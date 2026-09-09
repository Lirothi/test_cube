#ifndef SKY_IBL_COMMON_HLSLI
#define SKY_IBL_COMMON_HLSLI
// UE ReflectionEnvironmentShaders.usf:99-129. D3D cube face order, world Y-up.
float3 SkyCubeDirection(float2 uv, uint face)
{
    float2 p = uv * 2.0f - 1.0f;
    if (face == 0) return normalize(float3(1, -p.y, -p.x));
    if (face == 1) return normalize(float3(-1, -p.y, p.x));
    if (face == 2) return normalize(float3(p.x, 1, p.y));
    if (face == 3) return normalize(float3(p.x, -1, -p.y));
    if (face == 4) return normalize(float3(p.x, -p.y, 1));
    return normalize(float3(-p.x, -p.y, -1));
}
#endif
