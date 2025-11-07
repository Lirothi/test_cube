// RootSignature: CONSTANTS(b0,count=4) TABLE(UAV(u0))
#pragma pack_matrix(row_major)

struct InstanceData
{
    row_major float4x4 world;
    row_major float4x4 prevWorld;
    float rotationY;
    float3 pad_;
};

RWStructuredBuffer<InstanceData> gInstances : register(u0);

cbuffer AnimParams : register(b0)
{
    float deltaTime;
    float angularSpeed;
    uint instanceCount;
    float _pad;
}

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint idx = DTid.x;
    if (idx >= instanceCount)
    {
        return;
    }

    // --- Rotation around Y (radians)
    gInstances[idx].rotationY += (angularSpeed + (idx % 7) * 0.1f) * deltaTime;
    if (gInstances[idx].rotationY > 6.283185f)
    {
        gInstances[idx].rotationY -= 6.283185f;
    }
    float a = gInstances[idx].rotationY + idx;
    //a = 0;
    // Row-vector compatible R_y matrix
    float4x4 R =
    {
        cos(a), 0, sin(a), 0,
        0, 1, 0, 0,
       -sin(a), 0, cos(a), 0,
        0, 0, 0, 1
    };

    // --- Scale ---
    // Apply a global scale via globalScale,
    // and optionally multiply by per-instance data (treat scale=0 as 1)
    //float3 s = gInstances[idx].scale;
    //s = (s.x == 0 && s.y == 0 && s.z == 0) ? float3(1, 1, 1) : s;
    //s *= globalScale;
    float3 s = 1.0f;

    float4x4 S =
    {
        s.x, 0, 0, 0,
        0, s.y, 0, 0,
        0, 0, s.z, 0,
        0, 0, 0, 1
    };

    // --- Grid placement ---
    const float spacing = 1.3f;
    const uint gridSize = 10;
    uint x = idx % gridSize;
    uint y = idx / gridSize;

    float3 offset = float3(
        (x - (gridSize - 1) * 0.5f) * spacing,
        (y - (gridSize - 1) * 0.5f) * spacing,
        0
    );

    float4x4 T =
    {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        offset.x, offset.y, offset.z, 1
    };
    
    //float4x4 T =
    //{
    //    1, 0, 0, offset.x,
    //    0, 1, 0, offset.y,
    //    0, 0, 1, offset.z,
    //    0, 0, 0, 1
    //};

    gInstances[idx].prevWorld = gInstances[idx].world;
    gInstances[idx].world = mul(mul(S, R), T);
}
