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
    // lightInvPreExposure: the light target (lightIndex) is PRE-EXPOSED (lighting_cb.hlsli
    // lightPreExposure); a reused on-screen hit is brought back to radiance with this.
    // sunAngularRadius: the sun as UE's sphere light in the off-screen re-shade, as lighting_cs has it.
    uint skyIrradianceIndex; float skyIrradianceScale; float lightInvPreExposure; float sunAngularRadius;
    // P16.12: the ground's diffuse reflectance, mirroring lighting_cs. An off-screen hit is
    // re-shaded HERE, so leaving it out would make every reflection of a shaded surface about a
    // stop darker than the same surface seen directly -- the same half-a-pair defect P16.9 fixed
    // for the sky fill itself.
    // sunSoftRadius: UE's SoftSourceRadius of the sun, as lighting_cs has it (EvalBRDF).
    float3 groundAlbedoRgb; float sunSoftRadius;
    uint spotLightIndex; uint spotCount;      uint pointLightIndex; uint pointCount;
    // depthIndex reconstructs the PRIMARY surface (the reflector). screenDepthIndex is
    // the on-screen opaque depth used only for the fast-path visibility/depth-match — the
    // two differ for the glass reflection dispatch (primary = glass depth, screen = opaque
    // depth); for opaque reflections screenDepthIndex == depthIndex.
    // alphaMissKeep: stochastic coverage inflation for the foliage alpha test (see
    // RtAlphaCandidatePasses); frameSeed is FROZEN by the CPU (see the fill site).
    // alphaMode -- the REFLECTION ray's only (its shadow rays, the sky visibility and the GI see
    // the geometry, a leaf card solid: rt_geometry.hlsli RtVisibilityBlocked):
    // 0 = OFF (FORCE_OPAQUE traversal, foliage = solid cards, cheapest);
    // 1 = FIRST HIT (FORCE_OPAQUE traversal, then the COMMITTED hit is alpha-tested with the
    //     albedo sample the shading already fetches -- a transparent texel becomes a miss, so
    //     crowns get holes showing the sky fallback instead of what is truly behind them; the
    //     cost is the opaque path plus nothing);
    // 2 = FULL (non-opaque candidates alpha-test during traversal -- exact, the expensive one).
    uint screenDepthIndex; float alphaMissKeep; uint frameSeed; uint alphaMode;
    // Gather-then-shade payload slots. For the TRACE dispatch these are the payload UAV heap
    // indices; for the RESOLVE dispatch the payload SRV heap indices. The monolithic (glass)
    // dispatch leaves them 0 and never reads them. payloadSkyIndex: the off-screen hit's SKY
    // lighting, apart (RtTraceResult.skyPart). skyVisIndex (resolve only, 0 = none this frame):
    // the RT sky visibility the resolve scales that part by, at skyVisStrength (skyVis.diffuse).
    uint payloadRadianceIndex; uint payloadUvIndex; uint payloadSkyIndex; uint skyVisIndex;
    float skyVisStrength; uint _rtPad5; uint _rtPad6; uint _rtPad7;
}

SamplerState gSmp      : register(s0);
SamplerState gSmpPoint : register(s1);

// rt_trace's radiance alpha: 0 = no surface / ray missed / alpha hole (coverage 0); else the HIT DISTANCE (m),
// POSITIVE when radiancePart is the final radiance (off-screen re-shade), NEGATIVE when the resolve finishes it
// with + lightT.Sample(reuseUv). The distance is the glossy filter's (reflection_glossy_cs.hlsl): a GGX lobe
// spreads the reflected image over hitT x its angle. fp16 holds it to 0.05 % out to the ray's 1e4 m.
static const float kRtPayloadMiss = 0.0f;
float RtPayloadAlpha(float hitT, bool reuseScreen)
{
    const float t = max(hitT, 1.0e-3f); // never 0: that is the miss
    return reuseScreen ? -t : t;
}

// TLAS instance mask bits of the shadow casters (rt::kInstanceMaskShadow, solid and transmissive): an object with Cast
// Shadow off lacks both.
static const uint RT_MASK_SHADOW = kRtMaskShadow;

// Shadow ray toward a local light: 0 if an opaque TLAS triangle occludes the path
// to the light, else 1. Used for off-screen hit shading (sun + spot lights).
float RtTraceShadow(RaytracingAccelerationStructure tlas, float3 origin, float3 L, float maxDist,
                    float tMin = 1.0e-3f)
{
    // P16.11: same reasoning as the reflection ray -- TMin 0 lets a shadow ray hit its own
    // surface, which at a grazing sun angle is acne. The caller offsets along the normal; this
    // skips the distance needed to clear that offset at the ray's own angle.
    RayDesc sray; sray.Origin = origin; sray.Direction = L; sray.TMin = tMin; sray.TMax = maxDist;
    // The geometry, a leaf card solid: the alpha mode is the reflection ray's, not its shadow rays'
    // (rt_geometry.hlsli RtVisibilityBlocked).
    return RtVisibilityBlocked(tlas, sray, RT_MASK_SHADOW) ? 0.0f : 1.0f;
}

// The GATHER half's result. `radiancePart` is env (+ the full analytic direct term when the
// screen colour is not reusable); the consumer finishes with `+ lightT.Sample(reuseUv)` when
// `reuseScreen` -- addition commutes, so the split is bit-identical to the old inline sample.
struct RtTraceResult
{
    float3 radiancePart;
    bool   reuseScreen;
    float2 reuseUv;
    float  hitT;     // the ray's length to the hit (m): the glossy filter's lobe footprint
    // An off-screen hit's SKY lighting -- the sky's fill (with the ground bounce lighting_cs pairs with it) and its
    // reflection of the sky -- kept apart from radiancePart, because the sky reaches that point only where the sky
    // visibility says it does and the trace cannot afford rays at the hit to find out: the resolve scales it by the
    // REFLECTOR's sky visibility (rt_resolve_cs.hlsl). 0 for an on-screen hit: the lit HDR it reuses already holds
    // its sky, occluded.
    float3 skyPart;
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
    res.hitT = 0.0f;
    res.skyPart = float3(0.0f, 0.0f, 0.0f);

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

    res.hitT = q.CommittedRayT();
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
        BRDFResult br = EvalBRDF(bi, sunAngularRadius, sunSoftRadius);   // the sun's disc, as lighting_cs
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
        float3 skyFill;
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
            skyFill = albedo * (1.0f - metal) * (irradiance * skyIrradianceScale + groundBounceOverPi);
        }
        else
        {
            skyFill = albedo * ambientIntensity * lightRgb;
        }
        // The sky fill goes apart (RtTraceResult.skyPart): left in, it lit every off-screen hit as if
        // under the open sky -- the bathroom's walls behind the camera came back daylit and blue in
        // the panelling and the brass (2026-10-26, owner: "SSR is far more realistic").
        res.skyPart = skyFill * exposure;
        float3 col = float3(0.0f, 0.0f, 0.0f);
        if (br.NdotL > 0.0f) { col = (br.diffBRDF + br.specBRDF) * br.NdotL * lightRgb * shadow; }
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

    // --- Environment (skybox) reflection at an OFF-SCREEN hit -- what makes metals reflect the sky.
    // Sky lighting too, so it joins skyPart. An on-screen hit gets none: the lit HDR it reuses holds its
    // sky specular already (lighting_cs adds it, occluded by the sky visibility) -- adding this as well
    // counted the hit's sky twice, the second time unoccluded.
    if (!res.reuseScreen)
    {
        TextureCube skybox = ResourceDescriptorHeap[skyboxIndex];
        float3 Rhit = reflect(-V, N);
        float3 skyCol = skybox.SampleLevel(gSmp, Rhit, 0).rgb * skyboxIntensity;
        float3 F0 = lerp(kF0Dielectric, albedo, metal);
        float3 F = F_Schlick(saturate(dot(N, V)), F0);
        float gloss = saturate(1.0f - rough);
        res.skyPart += skyCol * F * gloss;
    }

    res.radiancePart = direct;
    return true;
}

// How much sky the REFLECTOR at this pixel sees (its RT sky visibility's bent-normal length, at skyVisStrength;
// 1 when none was traced this frame) -- what an off-screen hit's sky lighting (RtTraceResult.skyPart) is scaled
// by: the trace cannot afford rays at the hit to ask the hit itself, and the reflector stands in for it -- indoors
// both see the room's windows, outdoors both the open sky. Compose makes the same stand-in for a MISS, whose sky
// fallback it occludes with the reflector's visibility.
float RtReflectorSkyVisibility(uint2 pixel)
{
    if (skyVisIndex == 0u)
    {
        return 1.0f;
    }
    Texture2D skyVis = ResourceDescriptorHeap[skyVisIndex];
    const float2 uv = (float2(pixel) + 0.5f) / float2(outWidth, outHeight);
    const float4 bent = skyVis.SampleLevel(gSmpPoint, uv, 0);
    // .w 0 = the sky itself, nothing traced there
    return bent.w > 0.0f ? lerp(1.0f, saturate(length(bent.xyz)), saturate(skyVisStrength)) : 1.0f;
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
