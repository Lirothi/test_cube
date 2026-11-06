// The sun's shadow on a FORWARD surface of the transparent passes -- glass (glass.hlsl) and the ocean
// (ocean_surface_common.hlsli) -- with the same clipmap / cascade / SDSM arithmetic and the same numbers
// lighting_cs uses, all from the GlassView CB (b1). Include it after vsm_sample.hlsli, csm_sample.hlsli and
// glass_view_cb.hlsli. 1 = lit, 0 = shadowed.
#ifndef FORWARD_SUN_SHADOW_HLSLI
#define FORWARD_SUN_SHADOW_HLSLI

// smrtRays: the VSM's SMRT ray count, at most the frame's (smrtParams.x); 0 = its single-tap path.
float ForwardSunShadow(float3 Pws, float3 Nws, float NdotL, uint smrtRays,
                       Texture2D shadowAtlas, StructuredBuffer<uint> vsmPageTable, Texture2D vsmPool,
                       StructuredBuffer<SdsmPartition> sdsmPartitions,
                       SamplerComparisonState shadowSampler, SamplerState linearSampler)
{
    // Step 24f: VSM mode samples the directional clipmap (matches lighting_cs.hlsl); Legacy = cascades.
    if (GLASS_VIEW(vsmParams).x != 0.0f)
    {
        // P16.16: same arithmetic and the same numbers lighting_cs gets, or the surface shades against
        // a differently-biased shadow.
        VsmSmrtParams smrt;
        smrt.rayCount = min((uint)GLASS_VIEW(smrtParams).x, smrtRays);
        smrt.samplesPerRay = (uint)GLASS_VIEW(smrtParams).y;
        smrt.rayLengthScale = GLASS_VIEW(smrtParams).z;
        smrt.extrapolateMaxSlope = GLASS_VIEW(smrtParams).w;
        smrt.sourceRadius = GLASS_VIEW(smrtParams2).x;
        smrt.texelDitherScale = GLASS_VIEW(smrtParams2).y;
        smrt.clipResBias = GLASS_VIEW(smrtParams2).z;
        smrt.frameIndex = (uint)GLASS_VIEW(smrtParams2).w;
        smrt.adaptiveRayCount = 0u; // pixel shader: no wave-op early out (see VSM_SMRT_COMPUTE)
        return VsmClipmapShadow(Pws, Nws, GLASS_VIEW(camPosSky).xyz, GLASS_VIEW(clipmapParams).y,
                                GLASS_VIEW(clipmapParams).z, GLASS_VIEW(clipmapParams).w,
                                GLASS_VIEW(vsmParams).z, GLASS_VIEW(vsmParams).w,
                                GLASS_VIEW(invProj)._11, GLASS_VIEW(clipmapUvNormal),
                                normalize(-GLASS_VIEW(sunDirAmbient).xyz), smrt,
                                // No screen-space ray here: the transparent passes have no scene depth
                                // of their own to march, and a forward surface is not where contact
                                // detail is judged. Same scope note as the adaptive ray count above.
                                0.0f,
                                GLASS_VIEW(clipmapViewProj), vsmPageTable, vsmPool, shadowSampler);
    }

    // S3: the cascade path goes through the SHARED csm_sample.hlsli, same as lighting_cs -- with its
    // coarser-cascade fallback and blend band, so the surface agrees with the geometry next to it at
    // every cascade boundary.
    CsmParams p;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        p.lightViewProj[i] = GLASS_VIEW(lightViewProj)[i];
        p.scaleBias[i]     = GLASS_VIEW(cascadeScaleBias)[i];
    }
    p.splitsVS     = GLASS_VIEW(cascadeSplitsVS);
    p.depthBiasNDC = GLASS_VIEW(shadowBiasNDC);
    p.cascadeTexelWS = GLASS_VIEW(cascadeTexelWS);
    p.atlasSize    = GLASS_VIEW(shadowAtlasSizeInv).xy;   // xy = size, zw = 1/size (see the GlassView cbuffer)
    p.camPosWS     = GLASS_VIEW(camPosSky).xyz;
    p.camDirWS     = normalize(GLASS_VIEW(camDirWS).xyz);
    p.pcfRadius    = 1.0f;
    // S8: same derivation as lighting_cs -- shadowBiasNDC already scales with the cascade's world
    // texel, so it IS the transition width.
    [unroll]
    for (int t = 0; t < 4; ++t)
    {
        p.transitionScale[t] = 1.0f / max(1e-6f, GLASS_VIEW(shadowBiasNDC)[t]);
    }
    p.receiverBiasMin  = GLASS_VIEW(csmFilterMode).y;
    p.sharpen          = GLASS_VIEW(csmFilterMode).z;
    p.overBlurCorrect  = GLASS_VIEW(csmFilterMode).w;
    p.normalBiasTexels = GLASS_VIEW(csmFilterParams).w;
    p.farSplit             = GLASS_VIEW(csmFilterParams).x;
    p.blendFraction        = GLASS_VIEW(csmFilterParams).y;
    p.distanceFadeFraction = GLASS_VIEW(csmFilterParams).z;
    p.useGatherPcf     = (uint)GLASS_VIEW(csmFilterMode).x;
    p.evsmExponents    = float2(0.0f, 0.0f); // S16: see lighting_cb.hlsli
    // S15: same overlay as lighting_cb.hlsli's MakeCsmParams -- not optional, the surface has to agree
    // with the geometry beside it exactly.
    if (GLASS_VIEW(csmSdsmParams).x > 0.0f) { CsmApplySdsm(p, sdsmPartitions, (uint)GLASS_VIEW(csmSdsmParams).x); }

    int cascade;
    return CsmSampleShadow(p, shadowAtlas, shadowSampler, linearSampler, Pws, Nws, NdotL, cascade);
}

#endif // FORWARD_SUN_SHADOW_HLSLI
