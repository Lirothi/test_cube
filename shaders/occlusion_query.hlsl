// Occlusion plan S3a: the box draw between BeginQuery/EndQuery. UE's FOcclusionQueryVS
// (SceneOcclusion.cpp: TShaderMapRef<FOcclusionQueryVS>) is exactly this -- the 8 corners of a
// world-space AABB through the view-projection, no attributes; the pixel shader exists only for
// the OcclusionMeshes show flag there and does nothing here. The PSO around it (OcclusionQueries.cpp)
// tests depth GREATER_EQUAL without writing and writes no colour: the sample counter of the query
// is the whole output.
//
// viewProj is the JITTERED matrix of the frame: the depth these boxes are tested against was
// rasterised with it, and a box tested through a differently-jittered projection would meet a
// depth buffer shifted by up to half a pixel under it.

#define OCCLUSION_QUERY_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0)"

cbuffer OcclusionQueryCB : register(b0)
{
    row_major float4x4 viewProj;
};

struct VSIn
{
    float3 position : POSITION;
};

struct VSOut
{
    float4 position : SV_POSITION;
};

[RootSignature(OCCLUSION_QUERY_RS)]
VSOut VSMain(VSIn input)
{
    VSOut o;
    o.position = mul(float4(input.position, 1.0f), viewProj);
    return o;
}

[RootSignature(OCCLUSION_QUERY_RS)]
void PSMain(VSOut input)
{
}
