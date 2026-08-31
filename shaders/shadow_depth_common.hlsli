// S6: the bias applied where cascade depth is WRITTEN, plus the S7 pancake clamp.
//
// Shared by every shadow-depth VS. It deliberately does NOT include gbuffer_common.hlsli:
// shadow_indirect_csm.hlsl declares its own leaner PerObject/PerView, so everything arrives as an
// argument. Same discipline as WindTransformCore in that file — a value the two permutations could
// disagree about is a parameter, never a global read twice.
#ifndef SHADOW_DEPTH_COMMON_HLSLI
#define SHADOW_DEPTH_COMMON_HLSLI

// H        — clip position, i.e. mul(worldPos, lightViewProj). For an ortho light H.w == 1.
// normalWS — the vertex normal in world space (unnormalized is fine, see below).
// lightVP  — the same matrix. With row-vector math clip.z = dot(pos, (_13,_23,_33)) + _43, so the
//            column (_13,_23,_33) is the light axis scaled by the projection's z scale. UE read
//            the equivalent column straight out of their WorldToShadowMatrix and do NOT normalize
//            (ShadowDepthVertexShader.usf:77-79) — their matrix chain happens to leave that column
//            unit. Ours does not: OrthoOffCenterLH puts 1/(far-near) there. So normalize, which is
//            correct for BOTH and cannot silently turn NoL into 1/zRange.
// params   — (constBias, slopeBias, maxSlope, clampToNear).
//
// The atlas is DIRECT Z (cleared to 1, DepthFunc LESS_EQUAL), so the bias is ADDED: that pushes the
// written depth AWAY from the light, which is the direction that stops a receiver shadowing itself.
// UE add theirs to a reversed depth for the same effect.
float4 ApplyShadowDepthBias(float4 H, float3 normalWS, float4x4 lightVP, float4 params)
{
    // The gate covers ALL the normal math, and it is not a micro-optimization: every path other
    // than a Legacy cascade passes zeros here, and the shadow VS is the hottest vertex path in the
    // engine. Pancaking is tested separately below — it needs neither the normal nor the matrix.
    if (params.x != 0.0f || params.y != 0.0f)
    {
        const float3 lightAxis = normalize(float3(lightVP._13, lightVP._23, lightVP._33));
        const float  NoL = abs(dot(lightAxis, normalize(normalWS)));
        const float  maxSlope = params.z;
        // slope = tan(angle between the surface and the light). The clamp is mandatory: as NoL -> 0
        // the required bias goes to infinity. Verbatim UE (ShadowDepthVertexShader.usf:84).
        const float  slope = clamp(NoL > 1e-4f ? sqrt(saturate(1.0f - NoL * NoL)) / NoL : maxSlope,
                                   0.0f, maxSlope);
        H.z += params.x + params.y * slope;
    }

    // S7 pancaking: a caster in front of the near plane is pressed onto it instead of being clipped.
    // UE (ShadowDepthVertexShader.usf:66-70) run reverse-Z, so their `if (z > w) { z = 0.999999; w = 1; }`
    // is the mirror image of this. They clamp BEFORE adding the bias; we clamp after, which is the
    // safer order — the bias cannot push a pancaked vertex back out through the near plane.
    // Clamping in the VS returns the vertex inside the frustum, so DepthClipEnable stays as it is
    // (which matters: the indirect PSO's raster state is shared with the VSM pool twins). Same
    // side effect as UE's: a triangle with some vertices clamped and some not is deformed.
    if (params.w > 0.0f) { H.z = max(H.z, 1e-6f); }
    return H;
}

#endif // SHADOW_DEPTH_COMMON_HLSLI
