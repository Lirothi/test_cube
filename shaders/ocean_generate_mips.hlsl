// RootSignature: CONSTANTS(b0,count=8) TABLE(SRV(t0)) TABLE(UAV(u0, numDescriptors=4))

cbuffer MipParams : register(b0)
{
    uint SrcWidth;
    uint SrcHeight;
    uint ArrayCount;
    uint SrcMip;
    uint FirstDstMip;
    uint MipCount;
    uint FirstDstWidth;
    uint FirstDstHeight;
};

Texture2DArray<float4> Source : register(t0);
RWTexture2DArray<float4> Dest[4] : register(u0);

[numthreads(8, 8, 1)]
void GenerateMip(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.z >= ArrayCount)
    {
        return;
    }

    uint width = FirstDstWidth;
    uint height = FirstDstHeight;
    uint stride = 1;

    const uint2 srcMax = uint2(max(SrcWidth, 1u) - 1u, max(SrcHeight, 1u) - 1u);

    for (uint level = 0; level < MipCount; ++level)
    {
        const uint coverageX = width * stride;
        const uint coverageY = height * stride;
        if (dispatchThreadId.x >= coverageX || dispatchThreadId.y >= coverageY)
        {
            break;
        }

        if ((dispatchThreadId.x % stride) == 0 && (dispatchThreadId.y % stride) == 0)
        {
            const uint2 dstCoord = uint2(dispatchThreadId.x / stride, dispatchThreadId.y / stride);
            const uint levelDelta = (FirstDstMip + level) - SrcMip;
            const uint scale = 1u << levelDelta;
            const uint2 srcBase = dstCoord * scale;

            float4 sum = 0.0f;
            for (uint oy = 0; oy < scale; ++oy)
            {
                for (uint ox = 0; ox < scale; ++ox)
                {
                    uint2 coord = srcBase + uint2(ox, oy);
                    coord = uint2(min(coord.x, srcMax.x), min(coord.y, srcMax.y));
                    sum += Source.Load(int4(coord, dispatchThreadId.z, SrcMip));
                }
            }

            const float invCount = rcp(float(scale * scale));
            Dest[level][uint3(dstCoord, dispatchThreadId.z)] = sum * invCount;
        }

        stride <<= 1;
        width = max(1u, (width + 1u) >> 1);
        height = max(1u, (height + 1u) >> 1);
    }
}
