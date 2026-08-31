#define GBUFFER_CSM_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1)"
// Use the shared b0 (per-object) + b1 (per-view: viewProj) from gbuffer_common
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsli"
#include "shadow_depth_common.hlsli"

struct VSOutD { float4 H : SV_POSITION; };

[RootSignature(GBUFFER_CSM_RS)]
VSOutD VSMain(VSIn i)
{
    VSOutD o;
    // W5: same sway as the gbuffer BaseVS (shared ApplyWindWS/WindOffset), so this fallback path's
    // shadow tracks the swaying tree exactly like the indirect path does.
    float4 wp = mul(float4(i.P, 1.0f), world);
    wp.xyz += ApplyWindWS(i.P, wp.xyz, world, windStrength, i.WIND, windFoliage,
                          windTrunkStiff, windLeafScale, windGustMul, windTime);
    // S6: the normal is already in this layout (VSIn is the full PNTUV gbuffer input), so the
    // fallback path costs no extra fetch. World scale only skews the bias -- acceptable.
    const float3 nWS = mul(i.N, (float3x3)world);
    o.H = ApplyShadowDepthBias(mul(wp, viewProj), nWS, viewProj, float4(shadowConstBias, shadowSlopeBias, shadowMaxSlope, shadowClampNear));
    return o;
}

// Depth-only — pixel shader can remain empty
[RootSignature(GBUFFER_CSM_RS)]
void PSMain(VSOutD i) { }
