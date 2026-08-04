// The source mip is read through a UAV, not an SRV, and that is the point of this signature.
//
// Mip generation reads mip N and writes mips N+1.. of the SAME resource. The engine's barrier
// model is whole-resource, so the texture is entirely in UNORDERED_ACCESS for this pass — and
// binding an SRV over it is undefined: a resource cannot be a UAV and a shader resource at once.
// It happened to work, and GPU-based validation reported it every frame as id=1358. Reading the
// source through its own mip's UAV keeps the whole pass in ONE state and needs no barrier at all.
// Different mips are different SUBRESOURCES, so read-mip-N/write-mip-N+1 in one dispatch is fine.
//
// Registers matter here: `RenderContext::kMaxBindings` is 4, and Material::Bind SKIPS any table
// whose base register is >= 4 — silently. So Source takes u0 and Dest u1..u4 (base u1), rather
// than the more natural Dest at u0 with Source after it.
#define OCEAN_MIPS_RS "RootConstants(num32BitConstants=8, b0), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u1, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

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

// Bound to the SrcMip slice itself, so the load carries no mip index (`SrcMip` survives in the
// constants because the level-delta arithmetic below still needs it).
RWTexture2DArray<float4> Source : register(u0);
RWTexture2DArray<float4> Dest[4] : register(u1);

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_MIPS_RS)]
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
                    sum += Source[uint3(coord, dispatchThreadId.z)];
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
