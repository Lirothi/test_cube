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
// Runs before Main_TransparentFog, so the fog and the aerial perspective reach the reflection with
// the rest of the water.
//
// t0: GBAux (B = shading model, A = W on the water)
// t1: GB2   (on the water: W * sky * preExposure; R11G11B10F)
// t2: the water's screen-space reflection (premultiplied, RAW radiance, reflection resolution)
// u0: scene colour (pre-exposed HDR)

#define WATER_COMPOSITE_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

#include "utils.hlsli"

Texture2D GBAux : register(t0);
Texture2D GB2 : register(t1);
Texture2D WaterReflection : register(t2);
RWTexture2D<float4> SceneColor : register(u0);
SamplerState gSmpLinear : register(s0);

cbuffer WaterCompositeCB : register(b0)
{
    float2 invRenderSize;
    float  preExposure;
    uint   pad0;
};

[numthreads(8, 8, 1)]
[RootSignature(WATER_COMPOSITE_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    SceneColor.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) { return; }

    const float4 aux = GBAux.Load(int3(tid.xy, 0));
    if (DecodeShadingModel(aux.b) != kShadingModelWater) { return; }

    // The reflection is traced at its own resolution; bilinear, so a texel the water does not cover
    // (written 0 by the trace) fades the hit toward the sky the forward draw already has, not to black.
    const float2 uv = (float2(tid.xy) + 0.5f) * invRenderSize;
    const float4 ssr = WaterReflection.SampleLevel(gSmpLinear, uv, 0.0f);
    const float3 skyWeighted = GB2.Load(int3(tid.xy, 0)).rgb;

    float4 c = SceneColor[tid.xy];
    c.rgb = max(c.rgb + aux.a * preExposure * ssr.rgb - saturate(ssr.a) * skyWeighted, 0.0f);
    SceneColor[tid.xy] = c;
}
