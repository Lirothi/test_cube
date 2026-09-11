#ifndef SKY_ATMOSPHERE_HLSLI
#define SKY_ATMOSPHERE_HLSLI
// B1 transcription from UE 5.6 SkyAtmosphereCommon.ush and SkyAtmosphere.usf.
// All distances km, coefficients km^-1. World metres / kMetresPerKm at the caller.
// LUTs use planet-local Z-up, independent of our Y-up camera and reverse-Z depth.
// No View, pre-exposure, sky light or scene shadow dependency in these transfer LUTs.
static const float kMetresPerKm = 1000.0f;
static const float SkyPi = 3.14159265358979323846f;
static const float PlanetRadiusOffset = 0.001f; // SkyAtmosphereCommon.ush:24 (1 metre)

cbuffer SkyAtmosphereCB : register(b0)
{
    float4 AtmosphereRadii; // bottom, top, Rayleigh/Mie density exponential scales
    float4 RayleighScattering;
    float4 MieScattering; // w: g (isotropic in B1, used by B2)
    float4 MieAbsorption;
    float4 AbsorptionExtinction; // w: ozone layer split altitude
    float4 AbsorptionDensity; // ascending linear/constant, descending linear/constant
    float4 GroundAlbedo; // w: multi-scattering factor
#ifdef SKY_VIEW
    float4 SkySunDirection;
    float4 SkyIlluminance;
    float4 SkyExposure;
    float4 SkyPlanet;
#endif
#ifdef SKY_AERIAL
    float4x4 AerialInvView;
    float4x4 AerialInvProj;
    float4 AerialStart; // x: froxel far-plane view depth in km, y: view-distance scale (UE AerialPespectiveViewDistanceScale)
#endif
};

// SkyAtmosphereCommon.ush:169-212, Bruneton transmittance mapping. No sub-UV remap.
void fromTransmittanceLutUVs(out float viewHeight, out float mu, float bottom, float top, float2 uv)
{
    float H = sqrt(top * top - bottom * bottom);
    float rho = H * uv.y;
    viewHeight = sqrt(rho * rho + bottom * bottom);
    float dMin = top - viewHeight;
    float dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    mu = d == 0.0f ? 1.0f : (H * H - rho * rho - d * d) / (2.0f * viewHeight * d);
    mu = clamp(mu, -1.0f, 1.0f);
}

void getTransmittanceLutUvs(float viewHeight, float mu, float bottom, float top, out float2 uv)
{
    float H = sqrt(max(0.0f, top * top - bottom * bottom));
    float rho = sqrt(max(0.0f, viewHeight * viewHeight - bottom * bottom));
    float discriminant = viewHeight * viewHeight * (mu * mu - 1.0f) + top * top;
    float d = max(0.0f, -viewHeight * mu + sqrt(discriminant));
    float dMin = top - viewHeight;
    float dMax = rho + H;
    uv = float2((d - dMin) / (dMax - dMin), rho / H);
}

// Common.ush RayIntersectSphere + SkyAtmosphere.usf:140-163.
// Preserve signed roots; -1,-1 denotes a missed sphere.
float2 SkyRaySphere(float3 origin, float3 dir, float3 centre, float radius)
{
    float3 p = origin - centre;
    float a = dot(dir, dir);
    float b = 2.0f * dot(p, dir);
    float c = dot(p, p) - radius * radius;
    float delta = b * b - 4.0f * a * c;
    if (delta < 0.0f) return -1.0f;
    return (-b + float2(-1, 1) * sqrt(delta)) / (2.0f * a);
}
float SkyRaySphereNearest(float3 origin, float3 dir, float3 centre, float radius)
{
    float2 roots = SkyRaySphere(origin, dir, centre, radius);
    return roots.x < 0.0f ? roots.y : (roots.y < 0.0f ? roots.x : min(roots.x, roots.y));
}

// SkyAtmosphereCommon.ush:292-333. Keep the complete medium decomposition for later sky/AP.
struct MediumSampleRGB
{
    float3 Scattering, Absorption, Extinction;
    float3 ScatteringMie, AbsorptionMie, ExtinctionMie;
    float3 ScatteringRay, AbsorptionRay, ExtinctionRay;
    float3 ScatteringOzo, AbsorptionOzo, ExtinctionOzo;
    float3 Albedo;
};
MediumSampleRGB SampleAtmosphereMediumRGB(float3 p)
{
    float h = max(0.0f, length(p) - AtmosphereRadii.x);
    float mie = exp(AtmosphereRadii.w * h);
    float ray = exp(AtmosphereRadii.z * h);
    float ozo = h < AbsorptionExtinction.w
        ? saturate(AbsorptionDensity.x * h + AbsorptionDensity.y)
        : saturate(AbsorptionDensity.z * h + AbsorptionDensity.w);
    MediumSampleRGB s;
    s.ScatteringMie = mie * MieScattering.rgb;
    s.AbsorptionMie = mie * MieAbsorption.rgb;
    s.ExtinctionMie = s.ScatteringMie + s.AbsorptionMie;
    s.ScatteringRay = ray * RayleighScattering.rgb;
    s.AbsorptionRay = 0;
    s.ExtinctionRay = s.ScatteringRay;
    s.ScatteringOzo = 0;
    s.AbsorptionOzo = ozo * AbsorptionExtinction.rgb;
    s.ExtinctionOzo = s.AbsorptionOzo;
    s.Scattering = s.ScatteringMie + s.ScatteringRay;
    s.Absorption = s.AbsorptionMie + s.AbsorptionOzo;
    s.Extinction = s.ExtinctionMie + s.ExtinctionRay + s.ExtinctionOzo;
    s.Albedo = s.Scattering / max(0.001f, s.Extinction);
    return s;
}

// SkyAtmosphere.usf:433-466. Integrator domain clips against planet and atmosphere.
float SkyRayLength(float3 p, float3 dir, out float tBottom)
{
    float2 b = SkyRaySphere(p, dir, 0, AtmosphereRadii.x);
    float2 t = SkyRaySphere(p, dir, 0, AtmosphereRadii.y);
    tBottom = 0;
    if (all(t < 0)) return 0;
    if (all(b < 0)) return max(t.x, t.y);
    tBottom = max(0.0f, min(b.x, b.y));
    return tBottom;
}

#ifdef SKY_MULTISCATTER
Texture2D<float4> TransmittanceLutTexture : register(t0);
SamplerState TransmittanceSampler : register(s0);

float3 GetTransmittance(float mu, float height)
{
    float2 uv;
    getTransmittanceLutUvs(height, mu, AtmosphereRadii.x, AtmosphereRadii.y, uv);
    return TransmittanceLutTexture.SampleLevel(TransmittanceSampler, uv, 0).rgb;
}

// SkyAtmosphere.usf:398-808, fixed 15 samples, isotropic, unit white illuminance,
// Ground=true, MULTI_SCATTERING_POWER_SERIE=0 (the actual UE default at :733).
void IntegrateSkyMulti(float3 p, float3 dir, float3 lightDir, out float3 L, out float3 multiAs1)
{
    L = 0;
    multiAs1 = 0;
    if (dot(p, p) <= AtmosphereRadii.x * AtmosphereRadii.x) return;
    float bottom;
    float tMax = SkyRayLength(p, dir, bottom);
    float dt = tMax / 15.0f;
    float3 throughput = 1;
    [loop] for (uint i = 0; i < 15; ++i)
    {
        // SkyAtmosphere.usf:364, 595: DEFAULT_SAMPLE_OFFSET=0.3, not midpoint.
        float3 sampleP = p + dir * (tMax * (i + 0.3f) / 15.0f);
        MediumSampleRGB m = SampleAtmosphereMediumRGB(sampleP);
        float3 tr = exp(-m.Extinction * dt);
        float height = length(sampleP);
        float3 up = sampleP / height;
        float3 toLight = GetTransmittance(dot(lightDir, up), height);
        float planet = SkyRaySphereNearest(sampleP, lightDir, PlanetRadiusOffset * up, AtmosphereRadii.x);
        float3 S = (planet >= 0 ? 0.0f : 1.0f) * toLight * m.Scattering / (4.0f * SkyPi);
        multiAs1 += throughput * m.Scattering * dt; // UE :737, intentionally not analytic
        L += throughput * (S - S * tr) / max(m.Extinction, 1.e-9f); // :758
        throughput *= tr;
    }
    if (tMax == bottom) // UE :771-783: ground bounce does not enter multiAs1
    {
        float3 groundP = p + bottom * dir;
        float h = length(groundP);
        float mu = dot(lightDir, groundP / h);
        L += GetTransmittance(mu, h) * throughput * saturate(mu) * GroundAlbedo.rgb / SkyPi;
    }
}
#endif
#endif
