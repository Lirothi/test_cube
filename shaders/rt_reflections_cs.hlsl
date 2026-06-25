// Tier-2 hardware ray-traced reflections (S7 + S9 + S10).
//
// cs_6_6 inline RayQuery + SM6.6 dynamic resources. One SHARP reflection ray per
// GBuffer surface against the TLAS. On a hit, shade it to MATCH THE BASE PASS:
//   - bindless-fetch the interpolated normal + UV and the material (albedo
//     texture/tint, roughness, metalness);
//   - DIRECT lighting: if the hit reprojects onto the visible surface on screen,
//     sample the lit HDR buffer (exact); otherwise evaluate the same Cook-Torrance
//     BRDF as the base pass -- sun (lighting_cs) + spot lights (spotlight_cs) +
//     point lights (pointlight_cs) + ambient, with TLAS shadow rays for sun/spot;
//   - plus an ENVIRONMENT term: skybox reflection at the hit, Fresnel*gloss
//     (same as compose's spec) — this is what makes metals look metallic.
// On miss: coverage 0 -> compose's skybox fallback. Writes premultiplied
// (rgb, coverage) into the reflection target; blur + compose unchanged.
//
// Glossy/rough blur is NOT done here: stochastic glossy needs a real denoiser
// (DLSS Ray Reconstruction) — see plan S16. Reflections are sharp + clean.
#define RT_REFLECT_CS_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

#pragma pack_matrix(row_major)
#include "utils.hlsl"
#include "rt_geometry.hlsli" // GeometryInfo + bindless VB/IB loaders
#include "rt_lights.hlsli"   // SpotLightData/PointLightData + Eval helpers

cbuffer Probe : register(b0)
{
    float4x4 view;
    float4x4 proj;
    float4x4 invView;
    float4x4 invProj;
    float3 sunDirWS;     float ambientIntensity;
    float3 lightRgb;     float exposure;
    float depthA;        float depthB;        uint outWidth;       uint outHeight;
    uint tlasIndex;      uint lightIndex;     uint gb1Index;        uint depthIndex;
    uint reflectionUavIndex;    uint geomInfoIndex;  uint skyboxIndex;     float skyboxIntensity;
    uint spotLightIndex; uint spotCount;      uint pointLightIndex; uint pointCount;
}

SamplerState gSmp      : register(s0);
SamplerState gSmpPoint : register(s1);

// Shadow ray toward a local light: 0 if an opaque TLAS triangle occludes the path
// to the light, else 1. Used for off-screen hit shading (sun + spot lights).
float RtTraceShadow(RaytracingAccelerationStructure tlas, float3 origin, float3 L, float maxDist)
{
    RayDesc sray; sray.Origin = origin; sray.Direction = L; sray.TMin = 0.0f; sray.TMax = maxDist;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
    sq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFFu, sray);
    sq.Proceed();
    return (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
}

// Trace one reflection ray; on hit, return its radiance shaded like the base pass.
// camPos is the camera world position — the hit is shaded from the CAMERA view (as
// the base pass / lightT does) so the recompute is seamless with the screen sample.
bool TraceReflection(float3 origin, float3 dir, float3 camPos, out float3 radiance)
{
    radiance = float3(0.0f, 0.0f, 0.0f);

    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[tlasIndex];
    RayDesc ray; ray.Origin = origin; ray.Direction = dir; ray.TMin = 0.0f; ray.TMax = 1e4f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFFu, ray);
    while (q.Proceed()) {}
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) { return false; }

    float3 hitWS = origin + dir * q.CommittedRayT();

    // --- Bindless geometry + material at the hit ---
    StructuredBuffer<GeometryInfo> geom = ResourceDescriptorHeap[geomInfoIndex];
    GeometryInfo g = geom[q.CommittedInstanceID()];
    ByteAddressBuffer vb = ResourceDescriptorHeap[g.vbIndex];
    ByteAddressBuffer ib = ResourceDescriptorHeap[g.ibIndex];

    uint3 tri = LoadTriangle(ib, q.CommittedPrimitiveIndex(), g.indexIs32);
    float2 bary = q.CommittedTriangleBarycentrics();
    float  bw = 1.0f - bary.x - bary.y;
    float3 nObj = normalize(LoadNormal(vb, tri.x) * bw + LoadNormal(vb, tri.y) * bary.x + LoadNormal(vb, tri.z) * bary.y);
    float3x4 o2w = q.CommittedObjectToWorld3x4();
    float3 N = normalize(mul((float3x3)o2w, nObj));

    // Shade from the camera view (matches the base pass / lightT) so on-screen and
    // off-screen parts of a reflected object don't show a specular seam.
    float3 V = normalize(camPos - hitWS);
    if (dot(N, V) < 0.0f) { N = -N; } // orient toward the camera (base-pass front-face)

    float2 uvHit = LoadUV(vb, tri.x) * bw + LoadUV(vb, tri.y) * bary.x + LoadUV(vb, tri.z) * bary.y;
    float3 albedo = g.baseColor.rgb;
    if (g.albedoTexIndex != 0xFFFFFFFFu)
    {
        Texture2D albedoTex = ResourceDescriptorHeap[g.albedoTexIndex];
        albedo *= albedoTex.SampleLevel(gSmp, uvHit, 0).rgb;
    }
    // Roughness/metalness: from the MR texture (R=metal, G=rough, matching the
    // GBuffer) when the material has one, else the flat per-material values.
    float rough = saturate(g.roughness);
    float metal = saturate(g.metalness);
    if (g.mrTexIndex != 0xFFFFFFFFu)
    {
        Texture2D mrTex = ResourceDescriptorHeap[g.mrTexIndex];
        float2 mr = mrTex.SampleLevel(gSmp, uvHit, 0).rg;
        metal = saturate(mr.x);
        rough = saturate(mr.y);
    }

    // --- Direct lighting (matches the base pass / lighting_cs) ---
    float3 direct;
    Texture2D depthT = ResourceDescriptorHeap[depthIndex];
    float4 hv = mul(float4(hitWS, 1.0f), view);
    float4 hc = mul(hv, proj);
    bool haveScreen = false;
    if (hc.w > 1e-6f)
    {
        float2 huv = (hc.xy / hc.w) * float2(0.5f, -0.5f) + 0.5f;
        if (all(huv >= 0.0f) && all(huv <= 1.0f))
        {
            float sd = depthT.SampleLevel(gSmpPoint, huv, 0).r;
            if (sd > 1e-6f && abs(hv.z - depthB / (sd - depthA)) / max(depthB / (sd - depthA), 1e-3f) < 0.05f)
            {
                Texture2D lightT = ResourceDescriptorHeap[lightIndex];
                direct = lightT.SampleLevel(gSmp, huv, 0).rgb; // exact lit HDR
                haveScreen = true;
            }
        }
    }
    if (!haveScreen)
    {
        float3 hitOrigin = hitWS + N * 0.02f;

        // Sun (directional) + ambient -- multiplied by exposure, as lighting_cs.
        float3 L = normalize(-sunDirWS);
        float shadow = (dot(N, L) > 0.0f) ? RtTraceShadow(tlas, hitOrigin, L, 1e4f) : 1.0f;
        BRDFInput bi;
        bi.albedo = albedo; bi.rough = rough; bi.metal = metal; bi.N = N; bi.V = V; bi.L = L;
        BRDFResult br = EvalBRDF(bi);
        float3 col = albedo * ambientIntensity * lightRgb;
        if (br.NdotL > 0.0f) { col += (br.diffBRDF + br.specBRDF) * br.NdotL * lightRgb * shadow; }
        direct = col * exposure;

        // Spot lights -- accumulated WITHOUT exposure, matching spotlight_cs.hlsl.
        // Spot shadows use a TLAS ray (in place of the base pass's shadow atlas).
        if (spotCount > 0u)
        {
            StructuredBuffer<SpotLightData> spots = ResourceDescriptorHeap[spotLightIndex];
            for (uint si = 0u; si < spotCount; ++si)
            {
                float3 sl; float sdist;
                float3 rad = RtEvalSpotLight(spots[si], hitWS, sl, sdist);
                if (dot(rad, rad) <= 0.0f) { continue; }
                BRDFInput sbi; sbi.albedo = albedo; sbi.rough = rough; sbi.metal = metal; sbi.N = N; sbi.V = V; sbi.L = sl;
                BRDFResult sbr = EvalBRDF(sbi);
                if (sbr.NdotL <= 0.0f) { continue; }
                float ssh = RtTraceShadow(tlas, hitOrigin, sl, max(sdist - 0.04f, 0.0f));
                direct += (sbr.diffBRDF + sbr.specBRDF) * sbr.NdotL * rad * ssh;
            }
        }

        // Point lights -- no shadow + no exposure, matching pointlight_cs.hlsl.
        if (pointCount > 0u)
        {
            StructuredBuffer<PointLightData> points = ResourceDescriptorHeap[pointLightIndex];
            for (uint pi = 0u; pi < pointCount; ++pi)
            {
                float3 pl; float pdist;
                float3 rad = RtEvalPointLight(points[pi], hitWS, pl, pdist);
                if (dot(rad, rad) <= 0.0f) { continue; }
                BRDFInput pbi; pbi.albedo = albedo; pbi.rough = rough; pbi.metal = metal; pbi.N = N; pbi.V = V; pbi.L = pl;
                BRDFResult pbr = EvalBRDF(pbi);
                if (pbr.NdotL <= 0.0f) { continue; }
                direct += (pbr.diffBRDF + pbr.specBRDF) * pbr.NdotL * rad;
            }
        }
    }

    // --- Environment (skybox) reflection at the hit (same as compose's spec) ---
    // This is what makes metals reflect the sky and look metallic.
    TextureCube skybox = ResourceDescriptorHeap[skyboxIndex];
    float3 Rhit = reflect(-V, N);
    float3 skyCol = skybox.SampleLevel(gSmp, Rhit, 0).rgb * skyboxIntensity;
    float3 F0 = lerp(kF0Dielectric, albedo, metal);
    float3 F = F_Schlick(saturate(dot(N, V)), F0);
    float gloss = saturate(1.0f - rough);
    float3 env = skyCol * F * gloss;

    radiance = direct + env;
    return true;
}

[numthreads(8, 8, 1)]
[RootSignature(RT_REFLECT_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }

    RWTexture2D<float4> outTex = ResourceDescriptorHeap[reflectionUavIndex];
    Texture2D depthT = ResourceDescriptorHeap[depthIndex];
    Texture2D gb1    = ResourceDescriptorHeap[gb1Index];

    float2 uv = (float2(dtid.xy) + 0.5f) / float2(outWidth, outHeight);
    float depth = depthT.SampleLevel(gSmpPoint, uv, 0).r;

    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f); // no surface / miss -> skybox via compose
    if (depth > 1e-6f)
    {
        float3 N = normalize(gb1.SampleLevel(gSmp, uv, 0).rgb * 2.0f - 1.0f);
        float3 P = ReconstructPosWS(uv, depth, invProj, invView);
        float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;
        float3 R = reflect(normalize(P - camPos), N);

        float3 radiance;
        if (TraceReflection(P + N * 0.02f, R, camPos, radiance))
        {
            result = float4(radiance, 1.0f); // premultiplied, full coverage
        }
    }

    outTex[dtid.xy] = result;
}
