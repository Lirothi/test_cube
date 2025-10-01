// RootSignature: CBV(b0)
// Use the shared b0 from gbuffer_common: world/view/proj
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsl"

struct VSOutD { float4 H : SV_POSITION; };

VSOutD VSMain(VSIn i)
{
    VSOutD o;
    o.H = TransformPositionH(i.P, world, view, proj);
    return o;
}

// Depth-only — pixel shader can remain empty
void PSMain(VSOutD i) { }