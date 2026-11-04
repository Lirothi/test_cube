// The Mesh Editor's collision shape, drawn into the preview after the mesh, against its depth: what lies in front of the
// mesh in full, what lies behind it faint. A shape is mostly on or inside its mesh, so hiding what is behind would hide
// most of it. Each segment is a screen-space quad (an instance, six vertices), its edge smoothed over a pixel: a D3D line
// is one pixel wide and aliased. Compiled at runtime as vs_5_0 / ps_5_0 (EditorPreviewRenderer).

cbuffer LinesCB : register(b0)
{
    row_major float4x4 gViewProj; // mesh space -> clip, the picture's own
    float4 gViewport;             // xy = the target's size in pixels
};

// How far a behind-the-mesh line shows, of its full alpha.
static const float kBehindAlpha = 0.28f;
// Toward the eye, of the depth range: an edge lying on the surface must not flicker between the two passes. The range is
// fitted to the mesh (about 2.1 radii), so this is ~0.4 % of its radius.
static const float kDepthBias = 0.002f;

struct SegmentIn
{
    float3 a : POSITION0;
    float widthPixels : TEXCOORD0;
    float3 b : POSITION1;
    float4 color : COLOR0;
    uint corner : SV_VertexID;
};

struct LineVS
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float across : TEXCOORD0;    // pixels from the line's middle
    float halfWidth : TEXCOORD1;
};

LineVS LinesVS(SegmentIn input)
{
    static const float2 kCorners[6] = {
        float2(0.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
        float2(0.0f, -1.0f), float2(1.0f, 1.0f), float2(0.0f, 1.0f) };
    const float2 corner = kCorners[input.corner];

    float4 a = mul(float4(input.a, 1.0f), gViewProj);
    float4 b = mul(float4(input.b, 1.0f), gViewProj);
    // Behind the eye a perspective divide mirrors the point: the segment ends where it crosses.
    const float kNearestW = 1.0e-4f;
    if (a.w < kNearestW && b.w >= kNearestW) { a = lerp(a, b, (kNearestW - a.w) / (b.w - a.w)); }
    if (b.w < kNearestW && a.w >= kNearestW) { b = lerp(b, a, (kNearestW - b.w) / (a.w - b.w)); }

    LineVS output;
    output.color = input.color;
    output.halfWidth = 0.5f * input.widthPixels;
    // The smoothed edge needs a pixel beyond the width.
    const float reach = output.halfWidth + 1.0f;
    output.across = corner.y * reach;
    if (a.w < kNearestW && b.w < kNearestW)
    {
        output.position = float4(0.0f, 0.0f, 0.0f, 0.0f); // wholly behind the eye: nothing
        return output;
    }
    const float2 pixelsA = a.xy / a.w * 0.5f * gViewport.xy;
    const float2 pixelsB = b.xy / b.w * 0.5f * gViewport.xy;
    const float2 along = pixelsB - pixelsA;
    const float length = sqrt(dot(along, along));
    const float2 direction = length > 1.0e-4f ? along / length : float2(1.0f, 0.0f);
    const float2 sideways = float2(-direction.y, direction.x);
    // Half a pixel past each end, so two segments meet without a notch.
    const float2 offsetPixels = sideways * output.across + direction * (corner.x * 2.0f - 1.0f) * 0.5f;
    float4 position = corner.x < 0.5f ? a : b;
    position.xy += offsetPixels * 2.0f / gViewport.xy * position.w;
    position.z -= kDepthBias * position.w;
    output.position = position;
    return output;
}

float4 LinesFrontPS(LineVS input) : SV_Target
{
    const float coverage = saturate(input.halfWidth + 0.5f - abs(input.across));
    return float4(input.color.rgb, input.color.a * coverage);
}

float4 LinesBehindPS(LineVS input) : SV_Target
{
    const float coverage = saturate(input.halfWidth + 0.5f - abs(input.across));
    return float4(input.color.rgb, input.color.a * coverage * kBehindAlpha);
}
