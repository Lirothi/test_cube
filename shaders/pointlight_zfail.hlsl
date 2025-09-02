// RootSignature: CBV(b0)
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsl"

VSOut VSMain(VSIn v)
{
    VSOut o;
    o.H = TransformPositionH(v.P, world, view, proj);
    return o;
}
float4 PSMain() : SV_Target { return 0.0.xxxx; } // no color writes (mask=0 в PSO)