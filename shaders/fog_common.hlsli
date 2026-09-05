// Volumetric fog froxel grid (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part A): the depth
// distribution of the slices, a cell's world position, and the volume UV of a scene point --
// transcribed from UE (Common.ush:2379-2388 ComputeZSliceFromDepth / ComputeDepthFromZSlice,
// VolumetricFog.usf:57-76 ComputeCellTranslatedWorldPosition, HeightFogCommon.ush:477-492
// ComputeVolumeUVFromNDC). Shared by the scatter pass, the integration pass and every consumer
// (compose, the ocean surfaces, glass) so a slice means the same depth to all of them.
//
// GridZParams = (B, O, S): slice = log2(depth * B + O) * S, depth = (exp2(slice / S) - O) / B --
// UE's CalculateGridZParams (RenderUtils.h), solved on the CPU (SceneRenderer::FogGridZParams).
// `depth` is VIEW-space depth (distance along the view axis), as UE's SceneDepth.
#ifndef FOG_COMMON_HLSLI
#define FOG_COMMON_HLSLI

// Mirrors SceneResourceBootstrapper's FogPassConstants. The scatter pass binds it at b1 (b0 is the
// lighting cbuffer, lighting_cb.hlsli); the integration pass at b0.
#define FOG_CB(reg) \
cbuffer FogCB : register(reg) \
{ \
    float3 fogGridZParams;      /* (B, O, S) */ \
    float  fogNearFadeInInv;    /* 1 / near fade-in distance (UE VolumetricFogNearFadeInDistanceInv) */ \
    uint3  fogGridSize;         /* cells: (W, H, Z) */ \
    uint   fogFlags;            /* bit 0 history valid, bit 1 jitter on */ \
    float4x4 fogInvViewProjNoJitter;   /* clip -> world, this frame's UNJITTERED camera */ \
    float4x4 fogPrevViewProjNoJitter;  /* world -> clip, last frame's unjittered camera (history UV) */ \
    float4 fogProjZ;            /* (proj._33, proj._43, proj._34, proj._44): view z -> device z */ \
    float4 fogJitter;           /* xyz: this frame's cell offset (Halton 2,3,5), w: history weight */ \
    float4 fogMedium0;          /* density (per m, base-2 as the analytic model), height falloff, reference height, start distance */ \
    float4 fogMedium1;          /* albedo, extinction scale, phase g, sun scatter intensity */ \
    float4 fogMedium2;          /* sky scatter intensity, preExposure, 1 / previous preExposure, volumetric distance */ \
};

// UE ComputeZSliceFromDepth / ComputeDepthFromZSlice.
float FogSliceFromDepth(float3 zParams, float viewDepth)
{
    return log2(viewDepth * zParams.x + zParams.y) * zParams.z;
}

float FogDepthFromSlice(float3 zParams, float slice)
{
    return (exp2(slice / zParams.z) - zParams.y) / zParams.x;
}

// [0, 1] along the volume's W axis for a scene point, clamped as UE clamp their UV to the grid.
float FogNormalizedSlice(float3 zParams, uint gridZ, float viewDepth)
{
    return saturate(FogSliceFromDepth(zParams, viewDepth) / (float)gridZ);
}

// Volume UV of a screen point: screen uv (y down) + normalized slice. UE ComputeVolumeUVFromNDC.
float3 FogVolumeUV(float2 screenUv, float viewDepth, float3 zParams, uint gridZ)
{
    return float3(screenUv, FogNormalizedSlice(zParams, gridZ, viewDepth));
}

// Reverse-Z device depth from view-space depth, through the projection's z row/column
// (row-vector convention: clip.z = z * _33 + _43, clip.w = z * _34 + _44).
float FogDeviceZFromViewDepth(float4 projZ, float viewDepth)
{
    return (viewDepth * projZ.x + projZ.y) / max(viewDepth * projZ.z + projZ.w, 1.0e-6f);
}

// The world position of a cell (integer coordinate + [0,1]^3 offset inside it), UE's
// ComputeCellTranslatedWorldPosition: uv over the grid, NDC with y flipped, the slice's depth to
// device z, then the UNJITTERED inverse view-projection. `viewDepth` returns the slice depth.
float3 FogCellWorldPosition(uint3 coord, float3 cellOffset, uint3 gridSize, float3 zParams,
                            float4 projZ, float4x4 invViewProjNoJitter, out float viewDepth)
{
    const float2 volumeUV = (float2(coord.xy) + cellOffset.xy) / float2(gridSize.xy);
    const float2 ndc = (volumeUV * 2.0f - 1.0f) * float2(1.0f, -1.0f);
    viewDepth = FogDepthFromSlice(zParams, max((float)coord.z + cellOffset.z, 0.0f));
    const float deviceZ = FogDeviceZFromViewDepth(projZ, viewDepth);
    const float4 P = mul(float4(ndc, deviceZ, 1.0f), invViewProjNoJitter);
    return P.xyz / P.w;
}

float3 FogCellWorldPosition(uint3 coord, float3 cellOffset, uint3 gridSize, float3 zParams,
                            float4 projZ, float4x4 invViewProjNoJitter)
{
    float unused;
    return FogCellWorldPosition(coord, cellOffset, gridSize, zParams, projZ, invViewProjNoJitter, unused);
}

// Henyey-Greenstein in the textbook form (1 + g^2 - 2g cos)^-1.5: forward peak at cosTheta = +1,
// cosTheta = dot(direction TO the light, camera -> point). UE write the same function with +2g cos
// and negate the argument at the call (VolumetricFog.usf:888) -- one pair, transcribed as a pair.
float FogPhaseHG(float g, float cosTheta)
{
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * 3.14159265f * pow(max(1.0f + g2 - 2.0f * g * cosTheta, 1.0e-4f), 1.5f));
}

// The medium at a world point: extinction per metre, in the units the froxel integration uses
// (base e), derived from the analytic model's density (base 2 twice -- see inside).
float FogExtinctionAt(float worldY, float4 medium0, float extinctionScale)
{
    // The analytic model (atmosphere.hlsli, UE's CalculateLineIntegralShared) integrates the
    // density profile in BASE 2 and transmits in base 2 again: for a uniform medium its line
    // integral is density * ln2 per metre and T = exp2(-integral), i.e. e^(-(ln2)^2 * density * l).
    // The froxel integration is base e (exp(-sigma * step)), so parity with the analytic model --
    // the SAME air inside and outside the volume -- needs sigma = density * (ln2)^2. UE write 0.5
    // here (VolumetricFog.usf, GlobalDensity * 0.5, a 4 % approximation of 0.4805); the exact
    // constant is used so the two models agree by construction and the seam at the volume's far
    // plane carries no step.
    const float density = medium0.x * exp2(-medium0.y * (worldY - medium0.z));
    return max(density * 0.48045301f * extinctionScale, 0.0f);
}

#endif // FOG_COMMON_HLSLI
