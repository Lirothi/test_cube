#ifndef UDS_SKY_HLSLI
#define UDS_SKY_HLSLI

// ULTRA DYNAMIC SKY'S SKY MATERIAL OVER THE ATMOSPHERE (TimeOfDaySettings::skyUdsMaterial, 2026-11-04; owner: "fit us to
// UDS"). UDS draws its sky with a sphere whose material (Ultra_Dynamic_Sky_Mat, read node by node in the owner's UE 5.8)
// takes UE's SkyAtmosphereViewLuminance and:
//   * Scale_Intensity_Around_Sun: x Lit_Intensity (2.3 + 1.8 (u + v)) -- u = Sun_Centered_Gradient, (1 + dot(view, sun))
//     / 2 with the view's height above the sun's stretched by 1 + 2 (1 - sin(sun elevation)); v = Moon_Centered_Gradient,
//     (1 + dot(view, moon)) / 2 gated on by the sun under the horizon (saturate(-15 sin elevation));
//   * + SkyGlow: Night Sky Glow Color x lerp(1.3, 0.6, the sphere's vertex colour -- taken here as the view's height),
//     x Night Sky Glow Reflection Intensity (2) in a reflection capture;
//   * Contrast_Control: x max(1 + Extra_Contrast (min(In / Contrast Midpoint, 3) - 1), 0) x Overall Intensity.
// Its Sky_Material_Ambient_Fog (a lerp toward Ambient Color) is left out: measured against UE's bare SkyAtmosphere with the
// same numbers (scratchpad uds_ratio_lsq2.py, our own linear post process volume on top) UDS's sky is the atmosphere x
// OI x Lit (2.3 + 1.8 u) to 1-4 % by day -- 0.70 / 0.56 against 0.69 / 0.54 at noon -- with no trace of that lerp.
// The parameters are UDS's by the sun's elevation (TimeOfDay.cpp kUdsSky*), its absolute colours x our sun / its 5 lux.
// `sky` raw cd/m2; `dirWorld` the unit view direction (world, Y up); `glowScale` 1 for the drawn sky, 2 for a capture.
float3 UdsSkyLayer(float3 sky, float3 dirWorld, float4 params, float4 glow, float4 sunW, float4 moonW, float glowScale)
{
    if (glow.w <= 0.0f)
    {
        return sky;
    }
    const float3 L = sunW.xyz;
    const float k = 1.0f + 2.0f * (1.0f - saturate(L.y));
    const float3 W = normalize(float3(dirWorld.x, L.y + (dirWorld.y - L.y) * k, dirWorld.z));
    const float u = 0.5f * (1.0f + dot(W, L));
    const float v = 0.5f * (1.0f + dot(dirWorld, moonW.xyz)) * sunW.w * moonW.w;
    const float S = params.y * (2.3f + 1.8f * (u + v));
    const float3 inS = sky * S + glow.rgb * (lerp(1.3f, 0.6f, saturate(dirWorld.y)) * glowScale);
    const float3 contrast = max(1.0f + params.w * (min(inS / max(params.z, 1.0e-6f), 3.0f) - 1.0f), 0.0f);
    return inS * contrast * params.x;
}

#endif // UDS_SKY_HLSLI
