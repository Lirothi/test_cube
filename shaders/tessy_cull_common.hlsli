// Tessy (docs/tessy_plan.md): what the cluster CULLS share -- the instance record they read and the wind pad of a
// cluster's box (the derivation is in tessy_vsm_cull_cs.hlsl's header). Include after wind.hlsli and tessy_common.hlsli.
#ifndef TESSY_CULL_COMMON_HLSLI
#define TESSY_CULL_COMMON_HLSLI

// Matches render::InstancePerObject (224 bytes), as shadow_indirect_csm.hlsl reads it.
struct InstancePerObject
{
    float4x4 world;
    float4x4 prevWorld;
    float4   baseColor;
    float2   metalRough;
    float    alphaCutoff;
    float    mrMultiply;
    float4   texOffsScale;
    float4   texFlags;
    uint     objectId;
    uint3    _instPad1;
    float    windStrength;
    float    windLeafScale;
    float    windFoliage;
    float    windTrunkStiff;
};

static float2 g_lastDF = float2(0.0f, 0.0f); // the last WindPadBox's (D, F), for the validator

// Pads the cluster's world box (ctr, ext) by how far this frame's wind can carry its vertices (see the header):
// WindOffset's terms with the per-tree values exact and the per-vertex weights at the cluster's maxima. mode 1 =
// directional, 2 = the isotropic 2D + F (the A/B).
void WindPadBox(InstancePerObject ip, TessyCluster c, float4 w0, float4 w1, uint mode, inout float3 ctr, inout float3 ext)
{
    if (ip.windStrength <= 0.0f || w1.x <= 0.0f) { return; }
    const float3 origin = float3(ip.world._41, ip.world._42, ip.world._43);
    const float2 groveHash = WindHash2(origin.xz);
    const float amp = w1.x * ip.windStrength * (1.0f + kWindGroveAmp * (groveHash.x * 2.0f - 1.0f));
    const float stiff = max(ip.windTrunkStiff * (1.0f + kWindGroveStiff * (groveHash.y * 2.0f - 1.0f)), 0.05f);
    const float p = w0.x * w1.y + dot(origin.xz, float2(0.13f, 0.17f)) + groveHash.x * 6.2831853f;
    const float osc = sin(p) + 0.5f * sin(p * 2.3f + 1.7f);
    const float bend = (kWindBendSteady + kWindBendOsc * osc) * w1.z;
    const float f = WindBendProfile(c.windAlongMax) / stiff;
    const float leaf = ip.windFoliage * c.windLeafMax;
    const float want = abs(amp * bend * leaf * kWindFoliageStream);
    const float ks = kWindLeafShear * c.windLeafMax * ip.windLeafScale;
    const float stream = (ip.windLeafScale > 0.0f) ? min(want, ks) : want; // want*ks/(ks+want) <= both
    // 1 % and a millimetre over each bound: the vertex shader evaluates the same terms in another order.
    const float D = (abs(amp * bend * f) + stream) * 1.01f + 0.001f;
    const float F = abs(amp * w1.z * leaf * kWindFlutterAmp) * 1.01f + 0.001f;
    g_lastDF = float2(D, F); // the validator's breakdown

    const float3 rel = ctr - origin;                                   // the box about the pivot
    const float3 nearest = max(abs(rel) - ext, 0.0f);
    const float lMin = length(nearest);
    if (mode == 2u)
    {
        ext += 2.0f * D + F;
        return;
    }
    // Whatever the geometry: |c| <= 2D (above) and <= 2 L_max (both ends lie on the sphere of radius L). Each axis's
    // interval below is cut to this; a cluster within ~D of the pivot gets it alone.
    const float lMax = length(abs(rel) + ext);
    const float R = min(2.0f * D, 2.0f * lMax);
    const float3 flutter = float3(0.5f * F, F, 0.5f * F);
    if (lMin <= 1.05f * D)
    {
        ext += R + flutter;
        return;
    }
    const float3 w = float3(w0.z, 0.0f, w0.w);                         // unit wind direction
    // L > D keeps phi acute (d is not the triangle's longest side): sin(phi) <= d / (L - d), 1 - cos(phi) <= sin^2(phi).
    const float S = D * lMin / (lMin - D);
    const float T = D * D * lMin / ((lMin - D) * (lMin - D));
    // r^ = rel / |rel| per component over the box, |rel| in [lMin, lMax].
    const float3 rLo = rel - ext, rHi = rel + ext;
    const float3 nLo = clamp(rLo / select(rLo >= 0.0f, lMax, lMin), -1.0f, 1.0f);
    const float3 nHi = clamp(rHi / select(rHi >= 0.0f, lMin, lMax), -1.0f, 1.0f);
    // m = w.r^ (w.y = 0)
    const float2 mx = float2(min(w.x * nLo.x, w.x * nHi.x), max(w.x * nLo.x, w.x * nHi.x));
    const float2 mz = float2(min(w.z * nLo.z, w.z * nHi.z), max(w.z * nLo.z, w.z * nHi.z));
    const float mLo = mx.x + mz.x, mHi = mx.y + mz.y;
    float3 cLo, cHi;
    [unroll] for (int i = 0; i < 3; ++i)
    {
        // g = w_i - m r^_i over the intervals; c_i = s g - t r^_i with s in [0, S], t in [0, T].
        const float p0 = mLo * nLo[i], p1 = mLo * nHi[i], p2 = mHi * nLo[i], p3 = mHi * nHi[i];
        const float gLo = w[i] - max(max(p0, p1), max(p2, p3));
        const float gHi = w[i] - min(min(p0, p1), min(p2, p3));
        cLo[i] = max(min(0.0f, S * gLo) + min(0.0f, -T * nHi[i]), -R);
        cHi[i] = min(max(0.0f, S * gHi) + max(0.0f, -T * nLo[i]), R);
    }
    const float3 lo = ctr - ext + cLo - flutter;
    const float3 hi = ctr + ext + cHi + flutter;
    ctr = 0.5f * (lo + hi);
    ext = 0.5f * (hi - lo);
}

// Does the (padded) box hold a SAMPLE of the view it is drawn into? Its eight corners through `vp` (clip = mul(p, vp))
// onto the viewport's grid of `size` pixels (x right, y down, samples at k + 0.5): when the rectangle of their projections
// falls between two sample centres on either axis, no triangle inside the box can cover a sample and the rasterizer
// would draw nothing of it -- exact, whatever its triangles (the mesh shaders' per-triangle test, kTessyMsCullSmall, one
// level up). A box reaching w <= 0 has no rectangle: kept. The rectangle may leave the viewport: kept, conservative.
static const float kTessyRectMargin = 1.0f / 64.0f; // the rasterizer snaps to 1/256 px and does its own float math
bool TessyBoxHoldsSample(float3 ctr, float3 ext, float4x4 vp, float2 size)
{
    // clip(corner) = clip(centre) +- ext.x row 0 +- ext.y row 1 +- ext.z row 2 (row_major: mul(p, vp) = sum p_i row_i).
    const float4 hc = mul(float4(ctr, 1.0f), vp);
    const float4 ax = ext.x * vp[0], ay = ext.y * vp[1], az = ext.z * vp[2];
    float2 lo = 3.0e38f, hi = -3.0e38f;
    bool behind = false;
    [unroll] for (uint i = 0u; i < 8u; ++i)
    {
        const float4 h = hc + ((i & 1u) != 0u ? ax : -ax) + ((i & 2u) != 0u ? ay : -ay) + ((i & 4u) != 0u ? az : -az);
        behind = behind || h.w <= 1.0e-6f;
        const float2 ndc = h.xy / max(h.w, 1.0e-6f);
        const float2 px = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * size;
        lo = min(lo, px);
        hi = max(hi, px);
    }
    lo -= kTessyRectMargin;
    hi += kTessyRectMargin;
    return behind || all(ceil(lo - 0.5f) <= floor(hi - 0.5f));
}

#endif // TESSY_CULL_COMMON_HLSLI
