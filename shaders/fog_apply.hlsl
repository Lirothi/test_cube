// B6.1 -- ONE fog application for the water, in the same code as the opaque scene.
//
// UE draw the water first (DeferredShadingRenderer.cpp:3230 RenderSingleLayerWater), swap the depth
// buffer for the one that contains it, and only then run sky + height fog over the screen (:3262).
// The water surface is therefore fogged by the SAME screen-space passes as everything else, and
// SingleLayerWaterShading.ush carries no fog code at all. This pass is that step: our ocean writes
// depth and does not blend, which is exactly their SingleLayerWater, so it is in the depth buffer
// by the time this runs. Glass and particles do NOT write depth -- they are their translucency,
// they draw after this pass, and they fog themselves per material.
//
// THE MASK. Compose already fogged the opaque scene before the forward pass ran, so this pass must
// touch the water and nothing else, or the opaque half is fogged twice. `depthCopy` is the depth as
// it stood BEFORE the forward pass (the transparent pass snapshots it for refraction anyway), so a
// pixel whose depth CHANGED is exactly a pixel a depth-writing transparent won. No stencil, no
// second mask target, and it costs one point-sampled fetch.
//
// THE BLEND. Output is (in-scattered light, transmittance) and the PSO blends ONE / SRC_ALPHA, so
// the hardware computes dst * T + inscatter -- the same shape as UE's fog pass
// (TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_SourceAlpha> in RenderViewFog). The volumetric
// composite folds into the same pair: the ocean used to write
//     (color * T + inscatter * (1 - T)) * vol.a + vol.rgb
// which is exactly dst * (T * vol.a) + (inscatter * (1 - T) * vol.a + vol.rgb).
#define FOG_APPLY_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"
#pragma pack_matrix(row_major)

#include "utils.hlsli"
#include "ibl_common.hlsli"
#include "height_fog.hlsli"
#include "fog_common.hlsli"

Texture2D           DepthTex     : register(t0); // reverse-Z, AFTER the depth-writing transparents
Texture2D           DepthPrevTex : register(t1); // the same buffer BEFORE them -- the mask
TextureCube         SkyboxTex    : register(t2); // the whole-sphere sky picture (see FogSkyAlongView)
Texture3D<float4>   FogVolume    : register(t3); // plan A5 integrated froxels

SamplerState LinearClampSmp : register(s0);
SamplerState PointClampSmp  : register(s1);

// Mirrors SceneResourceBootstrapper's FogApplyConstants. CB-struct = MIRROR of the shader.
cbuffer FogApply : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float4x4 viewProjNoJitter;  // world -> clip, UNJITTERED: the volume's own UV, as compose uses
    float4   camPosWS;          // xyz; w unused
    float4   fogParams0;        // density, height falloff, reference height, start distance
    float4   fogParams1;        // max opacity, sun scatter strength, exponent, sun scatter start
    float4   fogParams2;        // sky blur, RESERVED (was back-scatter), directional-fade inv range, its bias
    float4   fogSunDir;         // xyz: direction TO the sun, already negated on the CPU as compose
                                //      receives it (SceneRenderer_Lighting.cpp) -- NOT the travel
                                //      direction the ocean shader used to be handed
    float4   fogSunColor;       // rgb: pre-exposed sun colour, as compose receives it
    float4   fogVolumeParams;   // on, far view depth, 1/preExposure, slice count
    float4   fogVolumeZParams;  // (B, O, S)
    float4   fogApplyMisc;      // x: sky intensity, y: debug view, z: preExposure, w: unused
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[RootSignature(FOG_APPLY_RS)]
VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    const float2 p = float2(vid == 2 ? 3.0f : -1.0f, vid == 1 ? 3.0f : -1.0f);
    o.pos = float4(p, 0.0f, 1.0f);
    o.uv = float2(p.x * 0.5f + 0.5f, 1.0f - (p.y * 0.5f + 0.5f));
    return o;
}

[RootSignature(FOG_APPLY_RS)]
float4 PSMain(VSOut i) : SV_Target
{
    const float2 uv = i.uv;
    const int3 px = int3((int)i.pos.x, (int)i.pos.y, 0);
    const float z = DepthTex.Load(px).r;
    const float zPrev = DepthPrevTex.Load(px).r;

    // ONLY the depth-writing transparents. `DepthPrevTex` is the depth as it stood BEFORE the
    // transparent pass, so a pixel whose depth CHANGED is exactly one the ocean (or another
    // depth-writing transparent) has just drawn. Opaque geometry and the sky were both answered by
    // compose; running the model over them here fogs them a SECOND time.
    //
    // This test was missing from b6.1 and the pass ran over the whole screen. On an open view that
    // is a full-screen second helping of height fog laid on top of compose's, and the sky gets it
    // computed from `z == 0` -- a reconstruction at the far plane, which does not follow elevation
    // the way compose's own sky ray does. The result was a haze band near the horizon that did not
    // line up with the horizon, which is exactly what the owner kept pointing at. It also silently
    // hijacked the transmittance and in-scattering debug views: their branches below return with
    // alpha 0, which REPLACES the pixel, so views 1 and 2 were showing this pass's numbers for the
    // whole frame while looking like compose's.
    // `z == zPrev` is the WHOLE test, and it must not be helped along with a "is this the sky"
    // epsilon. The sky is already excluded by it -- nothing wrote depth there, so the two values are
    // bit-identical -- while `z <= kEpsilon` is not a sky test at all: under reverse-Z, `z` is
    // near/distance, so with this project's 0.01 m near plane every surface past TEN KILOMETRES
    // falls under 1e-6. The ocean reaches well past that, and its far rows were dropped out of the
    // fog entirely: a three-row dark notch at the waterline with a step back to full fog where the
    // water crossed 10 km, measured at 19 levels of luminance.
    if (z == zPrev) { discard; }

    HeightFogParams fog;
    fog.density = fogParams0.x;
    fog.heightFalloff = fogParams0.y;
    fog.referenceHeight = fogParams0.z;
    fog.startDistance = fogParams0.w;
    fog.maxOpacity = fogParams1.x;
    fog.sunScatterStrength = fogParams1.y;
    fog.sunScatterExponent = fogParams1.z;
    fog.sunScatterStartDistance = fogParams1.w;

    const float3 Pw = ReconstructPosWS(uv, z, invProj, invView);
    const float3 toPoint = Pw - camPosWS.xyz;
    const float dist = length(toPoint);
    const float3 viewRay = dist > kEpsilon ? toPoint / dist : float3(0.0f, 0.0f, 1.0f);

    // Volume UV and view depth from the UNJITTERED clip position -- byte for byte what the ocean
    // shader did with its own world position, so the froxel lookup does not move by a jitter phase.
    const float4 fogClip = mul(float4(Pw, 1.0f), viewProjNoJitter);
    const float2 fogUv = (fogClip.xy / max(fogClip.w, 1.0e-4f)) * float2(0.5f, -0.5f) + 0.5f;
    const float4 vol = FogVolumeSampleAt(FogVolume, LinearClampSmp, fogUv, fogClip.w,
                                         fogVolumeParams, fogVolumeZParams);
    const float fogExclude = FogAnalyticExclude(fogVolumeParams, dist, fogClip.w);
    fog.startDistance = max(fog.startDistance, fogExclude);
    fog.sunScatterStartDistance = max(fog.sunScatterStartDistance, fogExclude);

    const float fogShared = HeightFogSharedIntegral(dist, camPosWS.y, Pw.y, fog);
    const float tau = HeightFogOpticalDepth(fogShared, dist, fog);
    const float minT = HeightFogMinTransmittance(tau, fog.maxOpacity);
    const float transmittance = HeightFogTransmittance(tau, minT);
    const float headroom = HeightFogHeadroom(transmittance, minT);
    const float3 skyAlongView = FogSkyAlongView(SkyboxTex, LinearClampSmp, viewRay, dist,
        HeightFogSkyRoughness(headroom, fogParams2.x), fogParams2.zw, fogApplyMisc.x);
    const float3 toSun = normalize(fogSunDir.xyz);
    const float3 inscatter = HeightFogInscatter(skyAlongView, fogSunColor.rgb,
                                                 dot(viewRay, toSun), fogShared, dist,
                                                 headroom, fog);

    const uint debugView = (uint)fogApplyMisc.y;
    if (debugView == 1u)
    {
        // Transmittance as a grey level. The blend still runs, so the surface has to be replaced
        // rather than modulated: alpha 0 drops what is underneath and rgb carries the value.
        return float4((transmittance * vol.a).xxx, 0.0f);
    }
    if (debugView == 2u)
    {
        return float4(inscatter * (1.0f - transmittance) * vol.a + vol.rgb, 0.0f);
    }
    // PRE-EXPOSURE. compose computes this in RAW radiance and scales on the way out; this pass
    // writes straight into the scene colour target, which is already pre-exposed. Without the same
    // scale the in-scattered light arrives in raw units - hundreds of times too bright - and the
    // frame blows to white. Transmittance is a ratio and is NOT scaled.
    return float4((inscatter * (1.0f - transmittance) * vol.a + vol.rgb) * fogApplyMisc.z,
                  transmittance * vol.a);
}
