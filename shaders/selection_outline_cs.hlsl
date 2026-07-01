#define SELECTION_OUTLINE_CS_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
// t0: stencil SRV of the deferred depth-stencil target
// u0: HDR scene color, modified in place before tonemap / DLSS output

Texture2D<uint2> SelectionStencil : register(t0);
RWTexture2D<float4> SceneColor : register(u0);

cbuffer SelectionOutlineCB : register(b0)
{
    float2 screenSize;
    uint selectedBit;
    uint outlineRadius;
    float4 outlineColor;
}

uint ReadStencil(int2 p)
{
    return SelectionStencil.Load(int3(p, 0)).g;
}

bool IsSelected(int2 p)
{
    return (ReadStencil(p) & selectedBit) != 0;
}

[numthreads(8, 8, 1)]
[RootSignature(SELECTION_OUTLINE_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint width = (uint)screenSize.x;
    const uint height = (uint)screenSize.y;
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    const int2 p = int2(dispatchThreadId.xy);
    if (IsSelected(p))
    {
        return;
    }

    const int radius = min(max((int)outlineRadius, 1), 8);
    bool touchesSelected = false;
    for (int oy = -radius; oy <= radius && !touchesSelected; ++oy)
    {
        for (int ox = -radius; ox <= radius; ++ox)
        {
            if (ox == 0 && oy == 0)
            {
                continue;
            }

            const int2 n = p + int2(ox, oy);
            if (n.x < 0 || n.y < 0 || n.x >= (int)width || n.y >= (int)height)
            {
                continue;
            }

            touchesSelected = touchesSelected || IsSelected(n);
        }
    }

    if (!touchesSelected)
    {
        return;
    }

    const float4 base = SceneColor[p];
    const float alpha = saturate(outlineColor.a);
    SceneColor[p] = float4(lerp(base.rgb, outlineColor.rgb, alpha), base.a);
}
