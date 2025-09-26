// RootSignature: CONSTANTS(b0,count=8) TABLE(SRV(t0)) TABLE(UAV(u1))

cbuffer MipParams : register(b0)
{
    uint SrcWidth;
    uint SrcHeight;
    uint DstWidth;
    uint DstHeight;
    uint ArrayCount;
    uint MipLevel;
    uint _pad0;
    uint _pad1;
};

Texture2DArray<float4> Source : register(t0);
RWTexture2DArray<float4> Dest : register(u1);

[numthreads(8, 8, 1)]
void GenerateMip(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DstWidth || dispatchThreadId.y >= DstHeight || dispatchThreadId.z >= ArrayCount)
    {
        return;
    }

    uint2 srcBase = dispatchThreadId.xy * 2;
    uint slice = dispatchThreadId.z;

    uint srcMaxX = SrcWidth - 1;
    uint srcMaxY = SrcHeight - 1;

    float4 sum = 0.0f;
    [unroll]
    for (uint oy = 0; oy < 2; ++oy)
    {
        [unroll]
        for (uint ox = 0; ox < 2; ++ox)
        {
            uint2 coord = uint2(min(srcBase.x + ox, srcMaxX), min(srcBase.y + oy, srcMaxY));
            sum += Source.Load(int4(coord, slice, 0));
        }
    }

    Dest[uint3(dispatchThreadId.xy, slice)] = sum * 0.25f;
}
