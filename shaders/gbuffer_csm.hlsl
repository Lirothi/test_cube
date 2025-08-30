// RootSignature: CBV(b0)
// Используем твой общий b0 из gbuffer_common: world/view/proj
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsl"

struct VSOutD { float4 H : SV_POSITION; };

VSOutD VSMain(VSIn i)
{
    VSOutD o;
    o.H = TransformPositionH(i.P, world, view, proj);
    return o;
}

// depth-only — PS можно оставить пустым
void PSMain(VSOutD i) { }