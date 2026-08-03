#ifndef RT_GEOMETRY_HLSLI
#define RT_GEOMETRY_HLSLI

// Shared bindless geometry definitions for the RT passes. Mirrors
// rt::GeometryInfoGPU (4x 16B rows) and the resident VertexPNTUV layout.
struct GeometryInfo
{
    uint   vbIndex;
    uint   ibIndex;
    uint   indexIs32;
    uint   albedoTexIndex; // 0xFFFFFFFF = none (use baseColor)
    float  roughness;
    float  metalness;
    uint   mrTexIndex;     // 0xFFFFFFFF = none (use flat roughness/metalness)
    uint   firstTri;       // B3: first triangle of this record's submesh range in the IB
    float4 baseColor;
    uint   mrMultiply;     // 0 = texture overrides values, 1 = texture * values
    uint   vertexStride;   // bytes per vertex, from Mesh::GetVertexStride()
    uint2  _pad;
};

// B3: records are per (instance, submesh) — the BLAS carries one geometry per submesh, so the
// record index is InstanceID (the mesh's FIRST record) + the committed GeometryIndex, and
// PrimitiveIndex is LOCAL to the geometry (offset by the record's firstTri when loading).
uint GeometryRecordIndex(uint instanceId, uint geometryIndex) { return instanceId + geometryIndex; }

// The vertex STRIDE now travels per-record in GeometryInfo (see vertexStride). It used to be a
// hardcoded 48 here, a fifth mirror of sizeof(VertexPNTUV) that nothing checked — when W7.1 appended
// COLOR_0 and the vertex became 52 bytes, every vertex except #0 decoded at the wrong offset and RT
// reflections shaded off garbage normals and UVs. Do NOT reintroduce a literal stride.
// The two OFFSETS are safe to keep here: they are positions of fields at the FRONT of the vertex,
// which the layout presets pin (COLOR_0 was deliberately appended at the end for exactly this reason).
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
float3 LoadNormal(ByteAddressBuffer vb, uint vertex, uint stride) { return asfloat(vb.Load3(vertex * stride + kRtNormalOffset)); }
float2 LoadUV(ByteAddressBuffer vb, uint vertex, uint stride)     { return asfloat(vb.Load2(vertex * stride + kRtUVOffset)); }

#endif // RT_GEOMETRY_HLSLI
