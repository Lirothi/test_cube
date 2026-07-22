#define POINTLIGHT_ZFAIL_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0)"
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsli"

[RootSignature(POINTLIGHT_ZFAIL_RS)]
VSOut VSMain(VSIn v)
{
    VSOut o;
    o.H = TransformPositionH(v.P, world, viewProj);
    return o;
}
[RootSignature(POINTLIGHT_ZFAIL_RS)]
float4 PSMain() : SV_Target { return 0.0.xxxx; } // no color writes (mask=0 in the PSO)
