#ifndef SKY_VIEW_MAPPING_HLSLI
#define SKY_VIEW_MAPPING_HLSLI
// UE SkyAtmosphereCommon.ush:37-38,194-225 and SkyAtmosphere.usf:228-267.
// Fixed world-aligned Z-up referential (world x,z,y), no camera yaw dependence.
static const float SkyViewPi = 3.14159265358979323846f;
static const float2 SkyViewSize = float2(192, 104);
float3 SkyViewUvToDir(float2 uv, float height, float bottom)
{
    uv = saturate((uv - 0.5f / SkyViewSize) * SkyViewSize / (SkyViewSize - 1.0f));
    float beta = acos(saturate(sqrt(max(0.0f, height * height - bottom * bottom)) / height));
    float horizon = SkyViewPi - beta;
    float angle = uv.y < 0.5f ? horizon * (1.0f - pow(1.0f - 2.0f * uv.y, 2.0f))
        : horizon + beta * pow(2.0f * uv.y - 1.0f, 2.0f);
    float longitude = uv.x * 2.0f * SkyViewPi;
    return float3(sin(angle) * cos(longitude), sin(angle) * sin(longitude), cos(angle));
}
float2 SkyViewDirToUv(float3 dir, float height, float bottom, bool ground)
{
    float beta = acos(saturate(sqrt(max(0.0f, height * height - bottom * bottom)) / height));
    float horizon = SkyViewPi - beta;
    float angle = acos(clamp(dir.z, -1.0f, 1.0f));
    float2 uv;
    uv.y = ground ? 0.5f + 0.5f * sqrt(saturate((angle - horizon) / max(beta, 1.e-6f)))
        : 0.5f * (1.0f - sqrt(saturate(1.0f - angle / horizon)));
    uv.x = (atan2(-dir.y, -dir.x) + SkyViewPi) / (2.0f * SkyViewPi);
    // Preserve UE's asymmetric unit/sub-UV transforms, including the +1 denominator.
    return (uv + 0.5f / SkyViewSize) * SkyViewSize / (SkyViewSize + 1.0f);
}
bool SkyViewIntersectsGround(float3 dir, float height, float bottom)
{
    return dir.z < 0.0f && height * height * (dir.z * dir.z - 1.0f) + bottom * bottom >= 0.0f;
}
#endif
