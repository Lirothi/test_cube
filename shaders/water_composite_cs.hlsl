// THE WATER'S REFLECTION COMPOSITE (docs/ocean_gbuffer_ssr_unification_plan.md S4; UE
// SingleLayerWaterComposite.usf). The water's forward draw already shaded the sky half of its
// reflection -- `reflected` was the environment cube alone -- and left in its G-buffer layer what
// that half weighed: GBAux.a = W (everything that multiplies `reflected` on its way into scene
// colour, pre-exposure excluded) and GB2 = W * sky * preExposure, the exact amount it added.
//
// The screen-space hit then REPLACES the sky where the ray found something, the way the forward
// shader's own `ssr.rgb + sky * (1 - ssr.a)` did before the trace moved here:
//
//     scene += W * preExposure * ssr.rgb  -  ssr.a * GB2
//
// so with no hit (ssr = 0) this adds nothing and the frame is the forward draw, bit for bit. The
// sky code stays in ONE place (the water's Reflection()), which is the point of not recomputing it
// here from the G-buffer normal and roughness the way UE do.
//
// THE BREAK-UP (2026-10-10). The trace runs at the reflection's resolution (0.5 of render) from a FLAT
// normal (ssr_cs flatNormal) at the wave's own point, so its image is a smooth mirror -- traced from
// the wave normal instead, every half-res texel's ray landed somewhere else and the bilinear upsample
// showed the reflected palms as visible texels. The wave shape comes back HERE, per full-res pixel:
// the mirror is read at uv + the planar reflection's old offset (OceanReflectionUvOffset, verbatim)
// from GB1's reflection normal -- the same normal the sky sample used, so one knob still drives both.
//
// Runs before Main_TransparentFog, so the fog and the aerial perspective reach the reflection with
// the rest of the water.
//
// t0: GBAux (B = shading model, A = W on the water)
// t1: GB1   (on the water: the reflection normal, the sky sample's)
// t2: GB2   (on the water: W * sky * preExposure; R11G11B10F)
// t3: depth after the water (the wave point, for the view direction)
// t4: the water's screen-space reflection (premultiplied, RAW radiance, reflection resolution)
// u0: scene colour (pre-exposed HDR)

#define WATER_COMPOSITE_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

#pragma pack_matrix(row_major) // invViewProj arrives from a row-major C++ mat4 (mul(v, M))
#include "utils.hlsli"

Texture2D GBAux : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D<float> DepthT : register(t3);
Texture2D WaterReflection : register(t4);
RWTexture2D<float4> SceneColor : register(u0);
SamplerState gSmpLinear : register(s0);

cbuffer WaterCompositeCB : register(b0)
{
    float4x4 invViewProj;      // clip -> world (the jittered projection the depth was drawn with)
    float3   camPos;
    float    roughnessDistance; // the ocean preset's roughnessDistance (the water's specularParams.z)
    float2   invRenderSize;
    float    preExposure;
    uint     uvBreakup;         // 1 = the flat mirror read at the UV offset; 0 = the trace already took
                                //     the wave normal (UE's way), read at the pixel itself
    // The prefilter (below): x, y = its Gaussian sigma across / along the screen's columns (render px), 0 = off; zw unused.
    float4   prefilter;
};

// The planar reflection's UV offset, VERBATIM from the water shader it lived in until S4: how far the
// wave's reflected ray swings from the flat one, in xz, scaled down with distance and up at grazing.
float2 OceanReflectionUvOffset(float3 viewDir, float viewDist, float3 adjustedNormal)
{
    float3 flatReflectDir = reflect(-viewDir, float3(0.0f, 1.0f, 0.0f));
    float3 waveReflectDir = reflect(-viewDir, adjustedNormal);
    float2 reflectionDelta = waveReflectDir.xz - flatReflectDir.xz;

    float distanceFade = saturate(viewDist / max(roughnessDistance, 1.0f));
    float grazing = saturate(1.0f - abs(waveReflectDir.y));
    float strength = lerp(0.08f, 0.025f, distanceFade) * lerp(0.45f, 1.0f, grazing) * 20;
    return reflectionDelta * strength;
}

float OceanReflectionEdgeFade(float2 uv)
{
    float2 edgeDist = min(uv, float2(1.0f, 1.0f) - uv);
    return saturate(min(edgeDist.x, edgeDist.y) * 64.0f);
}

// The reflected radiance a water pixel shows: the screen-space hit, and where its ray missed the sky the water's own draw
// took (GB2 = W x sky x preExposure, so the sky is GB2 / (W preExposure)). `uv` is where the reflection is read.
float3 WaterReflectedRadiance(int2 px, float2 reflectionUv, float weight)
{
    const float4 ssr = WaterReflection.SampleLevel(gSmpLinear, saturate(reflectionUv), 0.0f);
    const float3 sky = GB2.Load(int3(px, 0)).rgb / max(weight * preExposure, 1.0e-8f);
    return ssr.rgb + sky * (1.0f - saturate(ssr.a));
}

[numthreads(8, 8, 1)]
[RootSignature(WATER_COMPOSITE_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    SceneColor.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) { return; }

    const float4 aux = GBAux.Load(int3(tid.xy, 0));
    if (DecodeShadingModel(aux.b) != kShadingModelWater) { return; }

    const float2 uv = (float2(tid.xy) + 0.5f) * invRenderSize;
    const float depth = DepthT.Load(int3(tid.xy, 0));
    const float4 world = mul(float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f), invViewProj);
    const float3 toCamera = camPos - world.xyz / world.w;
    const float viewDist = length(toCamera);
    const float3 viewDir = toCamera / max(viewDist, 1.0e-4f);
    const float3 normal = normalize(GB1.Load(int3(tid.xy, 0)).rgb * 2.0f - 1.0f);

    // Bilinear: a texel the water does not cover (written 0 by the trace) fades the hit toward the
    // sky the forward draw already has, not to black.
    const float2 reflectionUv = uvBreakup != 0u ? uv + OceanReflectionUvOffset(viewDir, viewDist, normal) : uv;
    const float edgeFade = uvBreakup != 0u ? OceanReflectionEdgeFade(reflectionUv) : 1.0f;
    const float4 ssr = WaterReflection.SampleLevel(gSmpLinear, saturate(reflectionUv), 0.0f) * edgeFade;
    const float3 skyWeighted = GB2.Load(int3(tid.xy, 0)).rgb;

    float4 c = SceneColor[tid.xy];

    // THE PREFILTER (2026-11-02, owner: "zones of stretched pixels by the shore, even without DLSS"). Each water pixel
    // casts ONE mirror ray, from a normal whose ripples near the shore are finer than the pixel: neighbouring rays land on
    // the shaded beach, a trunk, the sky between the crowns -- the reflection is sampled far more sparsely than it varies,
    // and it aliases into runs of pixels (raw without DLSS; under DLSS, which cannot follow ripples that run at their
    // phase speed, into stretched smears). With the waves held DLSS's own jittered frames add up to a clean reflection,
    // which is what this does in space, for the frame at hand -- a pixel reconstruction filter, as a TAA resolve's spatial
    // kernel: the reflected radiance (the hit, and the sky where the ray missed) averaged over the neighbouring water by a
    // Gaussian of prefilter.xy px. Wider across the columns than along them: at a grazing view the ripples are horizontal
    // slivers, and a tall kernel would merge neighbouring crests. Everywhere on the water, not gated: the reflected ray
    // steps tens of the reflected image's pixels between neighbours on near and far water alike, and the second
    // difference of those steps (smooth sweep vs scatter) lit the near crests as much as the far grain -- nothing told the
    // two apart per pixel (2026-11-02).
    const float2 sigma = prefilter.xy;
    [branch] if (max(sigma.x, sigma.y) > 0.25f && aux.a > 1.0e-4f)
    {
        const int2 radius = int2(min(ceil(2.0f * sigma), float2(3.0f, 2.0f)));
        const float2 inv2s2 = 0.5f / max(sigma * sigma, 1.0e-4f);
        const float2 offset = reflectionUv - uv; // the break-up's (0 when the trace took the wave normal)
        float3 sum = 0.0f;
        float wsum = 0.0f;
        [loop] for (int dy = -radius.y; dy <= radius.y; ++dy)
        {
            [loop] for (int dx = -radius.x; dx <= radius.x; ++dx)
            {
                const int2 q = clamp(int2(tid.xy) + int2(dx, dy), int2(0, 0), int2(w - 1, h - 1));
                const float4 qa = GBAux.Load(int3(q, 0));
                if (DecodeShadingModel(qa.b) != kShadingModelWater || qa.a <= 1.0e-4f) { continue; }
                const float wq = exp(-(float)(dx * dx) * inv2s2.x - (float)(dy * dy) * inv2s2.y);
                const float2 quv = (float2(q) + 0.5f) * invRenderSize + offset;
                sum += wq * WaterReflectedRadiance(q, quv, qa.a);
                wsum += wq;
            }
        }
        const float3 filtered = sum / max(wsum, 1.0e-6f);
        // The sky this pixel's draw added, swapped for the filtered reflection; the break-up's screen-edge fade takes it
        // back toward that sky, as the plain path fades its hit.
        const float3 skyOnly = skyWeighted / max(aux.a * preExposure, 1.0e-8f);
        const float3 reflected = lerp(skyOnly, filtered, edgeFade);
        c.rgb = max(c.rgb - skyWeighted + aux.a * preExposure * reflected, 0.0f);
        SceneColor[tid.xy] = c;
        return;
    }
    c.rgb = max(c.rgb + aux.a * preExposure * ssr.rgb - saturate(ssr.a) * skyWeighted, 0.0f);
    SceneColor[tid.xy] = c;
}
