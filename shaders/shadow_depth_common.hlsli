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
// params   — (constBias, slopeBias, maxSlope, clampToNear). clampToNear < 0 is NOT a clamp: it marks a
//            perspective view (a spot light) and carries -n*f/(f-n); the biases are then world metres (below).
//            maxSlope < 0 marks a spot view with a LINEAR depth (ShadowLinearDepth below): this function then
//            leaves H alone -- the pixel shader writes the biased depth.
//
// The atlas is DIRECT Z (cleared to 1, DepthFunc LESS_EQUAL), so the bias is ADDED: that pushes the
// written depth AWAY from the light, which is the direction that stops a receiver shadowing itself.
// UE add theirs to a reversed depth for the same effect.
float4 ApplyShadowDepthBias(float4 H, float3 normalWS, float4x4 lightVP, float4 params)
{
    if (params.z < 0.0f) { return H; } // a linear-depth spot view: no clip-z bias, no pancaking (ShadowLinearDepth)
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
        const float  bias = params.x + params.y * slope;
        // params.w < 0: a PERSPECTIVE view (a spot light, local-light plan S1). The bias is then in WORLD metres along
        // the light's axis -- UE's spot bias, which it applies to a linear depth -- and -params.w is n*f/(f-n) of the
        // projection: an LH perspective's NDC depth moves by n*f/((f-n) w^2) per metre at view depth w, so the clip z
        // moves by that times w. A constant clip-z offset would instead grow ~w in the world.
        H.z += params.w < 0.0f ? (-params.w) * bias / max(H.w, 1e-4f) : bias;
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

// Local-light plan S4: a spot view's LINEAR depth, UE's (ShadowDepthVertexShader.usf:146-158 TransformShadowSpot hands
// the pixel shader the light-space depth and its bias; ShadowDepthPixelShader.usf:80-86 writes 1 - z / MaxSubjectDepth
// + bias into SV_Depth -- reversed; ours is direct, so the bias is added to (w - n) / (f - n)). params = (constBias m,
// slopeBias m, -1 / (f - n), n) from BuildSpotShadowViewCB; the slope is UE's tan to the light's axis clamped to 1
// (ComputeDepthBiasDirectionalSpot, r.Shadow.ShadowMaxSlopeScaleDepthBias 1). H.w is the view depth along the light's
// axis and is affine in the world, so the perspective-correct interpolant of this is exact at every pixel. Built only in
// the SHADOW_LINEAR_DEPTH permutations; their pixel shader returns saturate() of it as SV_Depth.
float ShadowLinearDepth(float4 H, float3 normalWS, float4x4 lightVP, float4 params)
{
    float bias = params.x;
    if (params.y != 0.0f)
    {
        const float3 lightAxis = normalize(float3(lightVP._13, lightVP._23, lightVP._33));
        const float  NoL = abs(dot(lightAxis, normalize(normalWS)));
        bias += params.y * clamp(NoL > 1e-4f ? sqrt(saturate(1.0f - NoL * NoL)) / NoL : 1.0f, 0.0f, 1.0f);
    }
    return (H.w + bias - params.w) * (-params.z);
}

#endif // SHADOW_DEPTH_COMMON_HLSLI
