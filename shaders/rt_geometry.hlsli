#ifndef RT_GEOMETRY_HLSLI
#define RT_GEOMETRY_HLSLI

// Shared bindless geometry definitions for the RT passes. Mirrors
// rt::GeometryInfoGPU (3x 16B rows) and the resident VertexPNTUV layout.
struct GeometryInfo
{
    uint   vbIndex;
    uint   ibIndex;
    uint   indexIs32;
    uint   albedoTexIndex; // 0xFFFFFFFF = none (use baseColor)
    float  roughness;
    float  metalness;
    uint   mrTexIndex;     // 0xFFFFFFFF = none (use flat roughness/metalness)
    uint   _pad1;
    float4 baseColor;
};

static const uint kRtVertexStride = 48u; // VertexPNTUV
static const uint kRtNormalOffset = 12u;
static const uint kRtUVOffset     = 40u;

uint LoadIndex16(ByteAddressBuffer ib, uint i)
{
    const uint byteOff = i * 2u;
    const uint word = ib.Load(byteOff & ~3u);
    return ((byteOff & 2u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
}
uint3 LoadTriangle(ByteAddressBuffer ib, uint prim, uint is32)
{
    if (is32 != 0u) { return ib.Load3(prim * 12u); }
    const uint b = prim * 3u;
    return uint3(LoadIndex16(ib, b), LoadIndex16(ib, b + 1u), LoadIndex16(ib, b + 2u));
}
float3 LoadNormal(ByteAddressBuffer vb, uint vertex) { return asfloat(vb.Load3(vertex * kRtVertexStride + kRtNormalOffset)); }
float2 LoadUV(ByteAddressBuffer vb, uint vertex)     { return asfloat(vb.Load2(vertex * kRtVertexStride + kRtUVOffset)); }

#endif // RT_GEOMETRY_HLSLI
