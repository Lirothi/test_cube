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
    float  alphaCutoff;    // Part C: < 0 = opaque; >= 0 = keep hit when baseColor.a*albedo.a >= cutoff
    uint   flags;          // bit0 = raster image wind-sways while RT geometry is rest-pose:
                           // do NOT reuse the on-screen colour for hits on this record, or the
                           // reflected image slides inside a static silhouette (re-shade instead)
                           // bit1 = kGeomFlagTwoSided: the material renders both faces
};

// rt::kGeomFlagTwoSided (BindlessTable.h): a hit on the back of such a surface is its other face, not the inside of a solid.
static const uint kGeomFlagTwoSided = 2u;

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

// Part C alpha test for CANDIDATE_NON_OPAQUE_TRIANGLE: only masked submeshes (BLAS geometry built
// without the OPAQUE flag) ever land here, so the opaque scene pays nothing. Mirrors the raster
// clip in gbuffer_common.hlsli — keep when baseColor.a * albedo.a >= cutoff — and mirrors UE's
// masked-material any-hit (RayTracingInstanceMask.cpp forces the AHS for masked segments; their
// inline scaffold commits from the same candidate case, TraceRayInline.ush). Alpha is sampled at
// mip 0, UE's ray-tracing default when ray-cone texture LOD is off (r.RayTracing.UseTextureLod=0).
uint RtWangHash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16);
    s *= 9u;
    s = s ^ (s >> 4);
    s *= 0x27d4eb2du;
    s = s ^ (s >> 15);
    return s;
}

// missKeep: stochastic coverage inflation for the alpha test. A 1-ray/px trace at reflection res
// UNDERSAMPLES thin foliage -- the sparse outer fronds drop below the temporal resolve's mean and
// the reflected crown reads visibly smaller than the crown itself. On an alpha FAILURE the hit is
// kept anyway with probability missKeep, decided by a hash of (pixel, seed, triangle). The seed is
// FROZEN by the caller: a static per-pixel pattern realises the inflation spatially and adds zero
// frame-to-frame dance, which measured strictly better than re-rolling per frame (0.43 vs 0.74
// resolved boil; see the frameSeed fill site). 0 = honest cutout, 1 = the old solid cards. A kept
// miss usually lands on the screen-reuse shading path, i.e. it picks up the full-res rasterized
// crown -- which is exactly the density the solid cards used to fake.
bool RtAlphaCandidatePasses(StructuredBuffer<GeometryInfo> geomBuf, SamplerState smp,
                            uint instanceId, uint geometryIndex, uint primitiveIndex, float2 bary,
                            float missKeep = 0.0f, uint raySeed = 0u)
{
    GeometryInfo g = geomBuf[GeometryRecordIndex(instanceId, geometryIndex)];
    if (g.alphaCutoff < 0.0f) { return true; } // defensively opaque (flag/record mismatch)
    float a = g.baseColor.a;
    if (g.albedoTexIndex != 0xFFFFFFFFu)
    {
        ByteAddressBuffer avb = ResourceDescriptorHeap[g.vbIndex];
        ByteAddressBuffer aib = ResourceDescriptorHeap[g.ibIndex];
        uint3 tri = LoadTriangle(aib, g.firstTri + primitiveIndex, g.indexIs32);
        float2 uv = LoadUV(avb, tri.x, g.vertexStride) * (1.0f - bary.x - bary.y) +
                    LoadUV(avb, tri.y, g.vertexStride) * bary.x +
                    LoadUV(avb, tri.z, g.vertexStride) * bary.y;
        Texture2D albedoTex = ResourceDescriptorHeap[g.albedoTexIndex];
        a *= albedoTex.SampleLevel(smp, uv, 0.0f).a;
    }
    if (a >= g.alphaCutoff) { return true; }
    if (missKeep <= 0.0f) { return false; }
    // Per-candidate roll: fold the triangle identity in so one ray crossing several leaves rolls
    // independently per leaf, not once for the whole traversal.
    const uint h = RtWangHash(raySeed ^ (primitiveIndex * 9781u) ^ (geometryIndex * 6271u) ^ instanceId);
    return (float(h & 0xFFFFu) * (1.0f / 65535.0f)) < missKeep;
}

// A VISIBILITY ray ("is anything in the way") -- every RT ray that is not a reflection's: the sky visibility, the GI's
// shadow and sky rays, a reflection hit's shadow rays. It sees the GEOMETRY, a leaf card as a solid card, no alpha test
// (owner, 2026-10-26): the alpha modes (rt_reflect_common.hlsli alphaMode) are the reflection rays' alone. Any triangle
// in the way blocks, so the first one the traversal meets ends it -- the answer cannot depend on the order.
//
// They used to follow the reflections' mode, and its mode 1 alpha-tested the hit the traversal met FIRST -- the BVH's
// order, not the ray's: the palms' wind BLAS are rebuilt every frame, so a crown turned "blocked" into "open" and back
// for every ray through it at once and the bathroom, lit through one window, pulsed (owner: "SkyVis dances inside the
// house"; per-pixel std over 11 shots 0.5 s apart, median 5.1 % / p90 19.9 %, the wind frozen 1.8 %).
bool RtVisibilityBlocked(RaytracingAccelerationStructure tlas, RayDesc ray, uint instanceMask)
{
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, instanceMask, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// A cutout (an alpha-tested slot -- a leaf card, a frond) as a PARTLY transparent layer for the visibility rays, at
// the opacity of the MESH it belongs to: mesh.json "rtCutoutOpacity" (absent = 1, every card solid; the palms 0.8),
// carried by its TLAS instance as 255 x (1 - opacity) in InstanceContributionToHitGroupIndex -- inline RayQuery has no
// hit groups (RtSceneAs, AccelerationStructure.cpp). Opaque geometry always blocks; so does every cutout of a mesh
// that does not say otherwise -- the bathroom's door, curtains and window wall are alpha-tested too, and one global
// 0.8 (the first version, 2026-10-27) let a fifth of the sky through them (owner: "walls must block fully").
// Why (owner: "make the leaves partly transparent for RT rays -- UE has the like"): as solid cards a palm crown shut
// out far more sky and sun than its fronds do (they cut 35-68 % of their cards away): the sand under the crowns lay in
// full GI shade, a leaf's underside saw no sky through the leaves above. A FIXED share `opacity` of the mesh's cutout
// triangles blocks, the rest lets every ray through: picked by hashing the triangle's own ids, so the answer depends
// neither on the traversal's order (what the first-found alpha test got wrong, above) nor on the frame. A roll per ray
// and frame (tried first) averages to the same occlusion but is noise every history has to average: the GI's probes
// behind the tent's wall walked +-1-2 % in metre-sized blotches (wind frozen, p90 1.5 -> 1.8-2.6 %; GI off: no
// difference). No texture fetch: the card's own alpha is the reflections' business (mode 2 cost the grove's SkyVis
// 2.0 ms).
bool RtCutoutTriangleBlocks(uint instanceId, uint geometryIndex, uint primitiveIndex, float cutoutOpacity)
{
    const uint h = RtWangHash((primitiveIndex * 9781u) ^ (geometryIndex * 6271u) ^ (instanceId * 2654435761u));
    return float(h & 0xFFFFFFu) * (1.0f / 16777216.0f) < cutoutOpacity;
}

// A non-opaque candidate's verdict: `packed` = its instance's InstanceContributionToHitGroupIndex (0 = a solid mesh).
bool RtCutoutCandidateBlocks(uint packed, uint instanceId, uint geometryIndex, uint primitiveIndex)
{
    const uint transparency = packed & 0xFFu;
    return transparency == 0u ||
           RtCutoutTriangleBlocks(instanceId, geometryIndex, primitiveIndex, 1.0f - float(transparency) * (1.0f / 255.0f));
}

// `meshOpacity` false (`rt.meshOpacity:0`, the A/B) = every card solid, the fast path above.
bool RtVisibilityBlocked(RaytracingAccelerationStructure tlas, RayDesc ray, uint instanceMask, bool meshOpacity)
{
    if (!meshOpacity)
    {
        return RtVisibilityBlocked(tlas, ray, instanceMask);
    }
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, instanceMask, ray);
    [loop]
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            RtCutoutCandidateBlocks(q.CandidateInstanceContributionToHitGroupIndex(), q.CandidateInstanceID(),
                                    q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex()))
        {
            q.CommitNonOpaqueTriangleHit(); // the first one blocks: ACCEPT_FIRST_HIT ends the search
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#endif // RT_GEOMETRY_HLSLI
