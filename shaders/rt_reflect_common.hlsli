#ifndef RT_REFLECT_COMMON_HLSLI
#define RT_REFLECT_COMMON_HLSLI

// The SHARED half of the RT reflection shaders (gather-then-shade split, async-compute prep).
//
// Three consumers, one source of truth:
//   rt_trace_cs.hlsl       -- the GATHER phase: traversal + full hit shading EXCEPT the lit-HDR
//                             screen sample; writes the payload. Inputs are TLAS/depth/gb1 only,
//                             so this pass can later move to the async compute queue and overlap
//                             the shadow/lighting work.
//   rt_resolve_cs.hlsl     -- the SHADE phase: payload + lightT -> reflection target. The ONLY
//                             consumer of the lighting pass's output.
//   rt_reflections_cs.hlsl -- the MONOLITHIC path, kept for the GLASS reflection dispatch (it
//                             runs after lighting anyway); combines the two phases inline.
//
// The shading here must keep MATCHING THE BASE PASS (lighting_cs / spotlight_cs / pointlight_cs)
// -- every constant and term is a mirror of that pass; change both or neither.

#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "rt_geometry.hlsli" // GeometryInfo + bindless VB/IB loaders
#include "rt_lights.hlsli"   // SpotLightData/PointLightData + Eval helpers

#define RT_REFLECT_CS_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

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
    // P16.9: the sky's cosine-convolved irradiance and the scale lighting_cs applies to it.
    // `skyIrradianceIndex == 0` means this sky has no F7 derivatives and the flat fallback stands.
    uint skyIrradianceIndex; float skyIrradianceScale; uint _rtPad0; uint _rtPad1;
    // P16.12: the ground's diffuse reflectance, mirroring lighting_cs. An off-screen hit is
    // re-shaded HERE, so leaving it out would make every reflection of a shaded surface about a
    // stop darker than the same surface seen directly -- the same half-a-pair defect P16.9 fixed
    // for the sky fill itself.
    float3 groundAlbedoRgb; float _rtPadGround;
    uint spotLightIndex; uint spotCount;      uint pointLightIndex; uint pointCount;
    // depthIndex reconstructs the PRIMARY surface (the reflector). screenDepthIndex is
    // the on-screen opaque depth used only for the fast-path visibility/depth-match — the
    // two differ for the glass reflection dispatch (primary = glass depth, screen = opaque
    // depth); for opaque reflections screenDepthIndex == depthIndex.
    // alphaMissKeep: stochastic coverage inflation for the foliage alpha test (see
    // RtAlphaCandidatePasses); frameSeed is FROZEN by the CPU (see the fill site).
    // alphaMode: 0 = OFF (FORCE_OPAQUE traversal, foliage = solid cards, cheapest);
    // 1 = FIRST HIT (FORCE_OPAQUE traversal, then the COMMITTED hit is alpha-tested with the
    //     albedo sample the shading already fetches -- a transparent texel becomes a miss, so
    //     crowns get holes showing the sky fallback instead of what is truly behind them; the
    //     cost is the opaque path plus nothing);
    // 2 = FULL (non-opaque candidates alpha-test during traversal -- exact, the expensive one).
    uint screenDepthIndex; float alphaMissKeep; uint frameSeed; uint alphaMode;
    // Gather-then-shade payload slots. For the TRACE dispatch these are the payload UAV heap
    // indices; for the RESOLVE dispatch the payload SRV heap indices. The monolithic (glass)
    // dispatch leaves them 0 and never reads them.
    uint payloadRadianceIndex; uint payloadUvIndex; uint _rtPad2; uint _rtPad3;
}

SamplerState gSmp      : register(s0);
SamplerState gSmpPoint : register(s1);

// Payload mode codes carried in rt_trace's radiance alpha (small integers are exact in fp16).
static const float kRtPayloadMiss = 0.0f;     // no surface / ray missed / alpha hole -> coverage 0
static const float kRtPayloadComplete = 1.0f; // radiancePart is the final radiance (off-screen re-shade)
static const float kRtPayloadReuse = 2.0f;    // finish with + lightT.Sample(reuseUv)

// Shadow ray toward a local light: 0 if an opaque TLAS triangle occludes the path
// to the light, else 1. Used for off-screen hit shading (sun + spot lights).
float RtTraceShadow(RaytracingAccelerationStructure tlas, float3 origin, float3 L, float maxDist,
                    float tMin = 1.0e-3f)
{
    // P16.11: same reasoning as the reflection ray -- TMin 0 lets a shadow ray hit its own
    // surface, which at a grazing sun angle is acne. The caller offsets along the normal; this
    // skips the distance needed to clear that offset at the ray's own angle.
    RayDesc sray; sray.Origin = origin; sray.Direction = L; sray.TMin = tMin; sray.TMax = maxDist;
    // Part C: FORCE_OPAQUE dropped — masked foliage (non-opaque BLAS geometry) must alpha-test,
    // or every leaf quad occludes as a solid card. Opaque geometry still never surfaces as a
    // candidate, and committing any passing candidate ends the search (ACCEPT_FIRST_HIT).
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> sq;
    sq.TraceRayInline(tlas, alphaMode != 2u ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE, 0xFFu, sray);
    StructuredBuffer<GeometryInfo> sgeom = ResourceDescriptorHeap[geomInfoIndex];
    while (sq.Proceed())
    {
        if (RtAlphaCandidatePasses(sgeom, gSmp, sq.CandidateInstanceID(), sq.CandidateGeometryIndex(),
                                   sq.CandidatePrimitiveIndex(), sq.CandidateTriangleBarycentrics()))
        {
            sq.CommitNonOpaqueTriangleHit();
        }
    }
    if (sq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) { return 1.0f; }
    // First-hit mode: alpha-test the committed hit only. A transparent first leaf lets the light
    // through even if a second leaf behind it would have blocked -- crown shadows lean lighter,
    // which is the cheap half of the trade.
    if (alphaMode == 1u &&
        !RtAlphaCandidatePasses(sgeom, gSmp, sq.CommittedInstanceID(), sq.CommittedGeometryIndex(),
                                sq.CommittedPrimitiveIndex(), sq.CommittedTriangleBarycentrics()))
    {
        return 1.0f;
    }
    return 0.0f;
}

// The GATHER half's result. `radiancePart` is env (+ the full analytic direct term when the
// screen colour is not reusable); the consumer finishes with `+ lightT.Sample(reuseUv)` when
// `reuseScreen` -- addition commutes, so the split is bit-identical to the old inline sample.
struct RtTraceResult
{
    float3 radiancePart;
    bool   reuseScreen;
    float2 reuseUv;
};

// Trace one reflection ray; on hit, shade it like the base pass EXCEPT the on-screen lit-HDR
// sample, which the caller applies (inline for the monolithic path, in rt_resolve for the split).
// camPos is the camera world position — the hit is shaded from the CAMERA view (as
// the base pass / lightT does) so the recompute is seamless with the screen sample.
bool TraceReflectionCore(float3 origin, float3 dir, float3 camPos, float tMin, uint raySeed,
                         out RtTraceResult res)
{
    res.radiancePart = float3(0.0f, 0.0f, 0.0f);
    res.reuseScreen = false;
    res.reuseUv = float2(0.0f, 0.0f);

    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[tlasIndex];
    // P16.11: TMin was 0, so the ray was free to hit the very triangle it started on. See the
    // call site for how the clearance is sized.
    RayDesc ray; ray.Origin = origin; ray.Direction = dir; ray.TMin = tMin; ray.TMax = 1e4f;
    // Part C: FORCE_OPAQUE dropped so masked foliage alpha-tests instead of reflecting as solid
    // quads. A committed candidate shrinks TMax and traversal continues to the true closest hit.
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(tlas, alphaMode != 2u ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE, 0xFFu, ray);
    StructuredBuffer<GeometryInfo> cgeom = ResourceDescriptorHeap[geomInfoIndex];
    while (q.Proceed())
    {
        if (RtAlphaCandidatePasses(cgeom, gSmp, q.CandidateInstanceID(), q.CandidateGeometryIndex(),
                                   q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
                                   alphaMissKeep, raySeed))
        {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) { return false; }

    float3 hitWS = origin + dir * q.CommittedRayT();

    // --- Bindless geometry + material at the hit ---
    StructuredBuffer<GeometryInfo> geom = ResourceDescriptorHeap[geomInfoIndex];
    GeometryInfo g = geom[GeometryRecordIndex(q.CommittedInstanceID(), q.CommittedGeometryIndex())];
    ByteAddressBuffer vb = ResourceDescriptorHeap[g.vbIndex];
    ByteAddressBuffer ib = ResourceDescriptorHeap[g.ibIndex];

    uint3 tri = LoadTriangle(ib, g.firstTri + q.CommittedPrimitiveIndex(), g.indexIs32);
    float2 bary = q.CommittedTriangleBarycentrics();
    float  bw = 1.0f - bary.x - bary.y;
    float3 nObj = normalize(LoadNormal(vb, tri.x, g.vertexStride) * bw +
                            LoadNormal(vb, tri.y, g.vertexStride) * bary.x +
                            LoadNormal(vb, tri.z, g.vertexStride) * bary.y);
    float3x4 o2w = q.CommittedObjectToWorld3x4();
    float3 N = normalize(mul((float3x3)o2w, nObj));

    // Shade from the camera view (matches the base pass / lightT) so on-screen and
    // off-screen parts of a reflected object don't show a specular seam.
    float3 V = normalize(camPos - hitWS);
    if (dot(N, V) < 0.0f) { N = -N; } // orient toward the camera (base-pass front-face)

    float2 uvHit = LoadUV(vb, tri.x, g.vertexStride) * bw +
                   LoadUV(vb, tri.y, g.vertexStride) * bary.x +
                   LoadUV(vb, tri.z, g.vertexStride) * bary.y;
    float3 albedo = g.baseColor.rgb;
    if (g.albedoTexIndex != 0xFFFFFFFFu)
    {
        Texture2D albedoTex = ResourceDescriptorHeap[g.albedoTexIndex];
        const float4 albedoSample = albedoTex.SampleLevel(gSmp, uvHit, 0);
        // First-hit alpha: the traversal was opaque, so the closest intersection may be a
        // transparent texel of a leaf card. Turning it into a MISS punches the hole the user
        // asked for -- the sky fallback shows through instead of the (unknown) content behind.
        // The alpha rides the very sample the shading fetches anyway, so this costs nothing.
        // The FILL knob applies here too, and matters MORE than in Full mode: a single-layer
        // test discards the crown's own depth, so without inflation the foliage reads eaten.
        // Same frozen per-pixel dither as RtAlphaCandidatePasses.
        if (alphaMode == 1u && g.alphaCutoff >= 0.0f &&
            albedoSample.a * g.baseColor.a < g.alphaCutoff)
        {
            const uint h = RtWangHash(raySeed ^ (q.CommittedPrimitiveIndex() * 9781u) ^
                                      (q.CommittedGeometryIndex() * 6271u) ^ q.CommittedInstanceID());
            if ((float(h & 0xFFFFu) * (1.0f / 65535.0f)) >= alphaMissKeep)
            {
                return false;
            }
        }
        albedo *= albedoSample.rgb;
    }
    // Roughness/metalness: from the MR texture (R=metal, G=rough, matching the
    // GBuffer) when the material has one, else the flat per-material values.
    float rough = saturate(g.roughness);
    float metal = saturate(g.metalness);
    if (g.mrTexIndex != 0xFFFFFFFFu)
    {
        Texture2D mrTex = ResourceDescriptorHeap[g.mrTexIndex];
        float2 mr = mrTex.SampleLevel(gSmp, uvHit, 0).rg;
        if (g.mrMultiply != 0u) { mr *= float2(metal, rough); }
        metal = saturate(mr.x);
        rough = saturate(mr.y);
    }

    // --- Direct lighting (matches the base pass / lighting_cs) ---
    float3 direct = float3(0.0f, 0.0f, 0.0f);
    Texture2D depthT = ResourceDescriptorHeap[screenDepthIndex]; // on-screen depth for the visibility match
    float4 hv = mul(float4(hitWS, 1.0f), view);
    float4 hc = mul(hv, proj);
    // RW reuse-deny (GeometryInfo.flags bit0): this hit's RASTER image is wind-swaying while its
    // RT geometry is rest-pose (sway toggle off, out of radius, or no free slot). The screen
    // sample under the projected hit would show the MOVING raster leaf, so the reflected image
    // slides inside a static silhouette. Skip the reuse; the analytic re-shade below is
    // consistent with the geometry the ray actually hit.
    if ((g.flags & 1u) == 0u && hc.w > 1e-6f)
    {
        float2 huv = (hc.xy / hc.w) * float2(0.5f, -0.5f) + 0.5f;
        if (all(huv >= 0.0f) && all(huv <= 1.0f))
        {
            float sd = depthT.SampleLevel(gSmpPoint, huv, 0).r;
            if (sd > 1e-6f && abs(hv.z - depthB / (sd - depthA)) / max(depthB / (sd - depthA), 1e-3f) < 0.05f)
            {
                // The exact lit HDR at huv finishes this radiance -- applied by the CALLER
                // (inline in the monolithic path, in rt_resolve for the split), because lightT
                // is the one input of this whole function that the lighting pass produces.
                res.reuseScreen = true;
                res.reuseUv = huv;
            }
        }
    }
    if (!res.reuseScreen)
    {
        float3 hitOrigin = hitWS + N * 0.02f;

        // Sun (directional) + ambient -- multiplied by exposure, as lighting_cs.
        float3 L = normalize(-sunDirWS);
        // P16.11: clear the 0.02 normal offset along THIS ray's direction.
        const float shadowTMin = 0.02f / max(dot(N, L), 0.05f);
        float shadow = (dot(N, L) > 0.0f) ? RtTraceShadow(tlas, hitOrigin, L, 1e4f, shadowTMin) : 1.0f;
        BRDFInput bi;
        bi.albedo = albedo; bi.rough = rough; bi.metal = metal; bi.N = N; bi.V = V; bi.L = L;
        BRDFResult br = EvalBRDF(bi);
        // P16.9 -- THE SAME SKY FILL THE MAIN PASS USES.
        //
        // This branch re-shades a hit that is OFF SCREEN, and it was the ONLY lighting path that
        // never got F8's sky-irradiance branch: it still lit with `ambient * sunColour`, the legacy
        // fraction knob. While the two were the same order nobody could tell. Once the sky was
        // authored in lux they were not: on `ssr_bronze_palms` this term gave 1592 against the
        // 6051 lx the very same leaf underside receives on screen, so every reflection of anything
        // the camera could not see came back four times too dark -- black, after the tone curve.
        //
        // That is also why the defect was BIMODAL. A hit that IS on screen reuses the frame's own
        // colour and stayed correct and sharp; only the off-screen re-shade went black, and those
        // are the blurry ones.
        float3 col;
        if (skyIrradianceIndex != 0u)
        {
            TextureCube skyIrradiance = ResourceDescriptorHeap[skyIrradianceIndex];
            const float3 irradiance = skyIrradiance.SampleLevel(gSmp, N, 0).rgb;
            // P16.12 ground bounce, the same two-half fill lighting_cs builds. Kept inline rather
            // than shared because the two passes reach their cube through different bindings; the
            // ARITHMETIC must stay identical, so change both or neither.
            float3 groundBounceOverPi = 0.0f.xxx;
            if (dot(groundAlbedoRgb, groundAlbedoRgb) > 0.0f)
            {
                const float3 sunOnGroundOverPi = lightRgb * saturate(-sunDirWS.y) * kInvPi;
                const float3 skyOnGroundOverPi =
                    skyIrradiance.SampleLevel(gSmp, float3(0.0f, 1.0f, 0.0f), 0).rgb *
                    skyIrradianceScale;
                groundBounceOverPi = groundAlbedoRgb * (sunOnGroundOverPi + skyOnGroundOverPi) *
                                     ((1.0f - N.y) * 0.5f);
            }
            col = albedo * (1.0f - metal) * (irradiance * skyIrradianceScale + groundBounceOverPi);
        }
        else
        {
            col = albedo * ambientIntensity * lightRgb;
        }
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
                const float spotTMin = 0.02f / max(dot(N, sl), 0.05f); // P16.11
                float ssh = RtTraceShadow(tlas, hitOrigin, sl, max(sdist - 0.04f, 0.0f), spotTMin);
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

    res.radiancePart = direct + env;
    return true;
}

// The primary-surface half of the dispatch: reconstruct the reflector at this pixel and set up
// the mirror ray. Shared verbatim by the monolithic and the trace CS so the two can never
// disagree about the ray they cast. Returns false when there is no surface (sky pixel).
bool RtSetupReflectionRay(uint2 pixel, out float3 origin, out float3 dir, out float3 camPos,
                          out float tMin, out uint raySeed)
{
    origin = float3(0.0f, 0.0f, 0.0f);
    dir = float3(0.0f, 0.0f, 1.0f);
    camPos = float3(0.0f, 0.0f, 0.0f);
    tMin = 0.0f;
    raySeed = 0u;

    Texture2D depthT = ResourceDescriptorHeap[depthIndex];
    Texture2D gb1    = ResourceDescriptorHeap[gb1Index];

    float2 uv = (float2(pixel) + 0.5f) / float2(outWidth, outHeight);
    float depth = depthT.SampleLevel(gSmpPoint, uv, 0).r;
    if (depth <= 1e-6f) { return false; }

    float3 N = normalize(gb1.SampleLevel(gSmp, uv, 0).rgb * 2.0f - 1.0f);
    float3 P = ReconstructPosWS(uv, depth, invProj, invView);
    camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;
    float3 R = reflect(normalize(P - camPos), N);

    // P16.11 -- SELF-INTERSECTION. `P` is RECONSTRUCTED FROM THE DEPTH BUFFER, so it carries an
    // error that grows with distance and is QUANTISED by the depth format. A fixed 0.02 along
    // the normal cannot cover that, and at a grazing angle it buys only 0.02/sin(theta) of
    // clearance ALONG the ray -- so on a large flat mirror the ray dips back under the plane and
    // hits the floor it came from. That is the evenly spaced, perspective-converging orange
    // banding across the bronze: the floor reflecting ITSELF, in bands set by where the depth
    // reconstruction happens to land above or below the true plane.
    //
    // Two parts, because there are two failures. The normal offset scales with view distance,
    // which is where the reconstruction error lives; and TMin skips the distance the ray needs
    // to actually clear that offset at ITS angle, which is the part a normal offset alone
    // cannot do. The cosine is floored so a ray exactly along the surface still gets a finite,
    // large clearance instead of an infinite one.
    const float viewDist = length(P - camPos);
    const float nOffset  = max(0.02f, viewDist * 0.002f);
    const float cosGraze = max(dot(N, R), 0.05f);
    raySeed = (pixel.x * 1973u) ^ (pixel.y * 9277u) ^ (frameSeed * 26699u);
    origin = P + N * nOffset;
    dir = R;
    tMin = nOffset / cosGraze;
    return true;
}

#endif // RT_REFLECT_COMMON_HLSLI
