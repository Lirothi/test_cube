#ifndef SKY_ATMOSPHERE_HLSLI
#define SKY_ATMOSPHERE_HLSLI
// B1 transcription from UE 5.6 SkyAtmosphereCommon.ush and SkyAtmosphere.usf.
// All distances km, coefficients km^-1. World metres / kMetresPerKm at the caller.
// LUTs use planet-local Z-up, independent of our Y-up camera and reverse-Z depth.
// No View, pre-exposure, sky light or scene shadow dependency in these transfer LUTs.
static const float kMetresPerKm = 1000.0f;
static const float SkyPi = 3.14159265358979323846f;
static const float PlanetRadiusOffset = 0.001f; // SkyAtmosphereCommon.ush:24 (1 metre)

// SKY_CB_EXTERNAL: the includer declares these fields itself, appended to its own cbuffer (the
// cloud capture, cloud_common.hlsli CLOUD_WITH_SKY_CB); the names must match this block.
#ifndef SKY_CB_EXTERNAL
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
    // Time-of-day plan N1: light 1, the moon (SkyViewFrameData). Zero illuminance = no second light.
    float4 SkyLight1Direction;   // w: disc angular radius (0 in the captures)
    float4 SkyLight1Illuminance; // outer-space RGB lux as lit now; w: earthshine fraction of the full disc
    float4 SkyMoonDisc;          // the FULL moon's outer-space RGB lux; w: phase angle (rad)
    float4 SkyMoonDisc2;         // x: the disc's lit scale; y: 1 = albedo/normal texture ready
    float4 SkyNightGlow;         // N2: rgb the airglow's zenith radiance (cd/m2); w the stars' + Milky Way's mean
#endif
#ifdef SKY_AERIAL
    float4x4 AerialInvView;
    float4x4 AerialInvProj;
    float4 AerialStart; // x: froxel far-plane view depth in km, y: view-distance scale (UE AerialPespectiveViewDistanceScale)
#endif
};
#endif // SKY_CB_EXTERNAL

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

#ifdef SKY_VIEW
// Time-of-day plan N1: the atmosphere's TWO lights, light 0 the sun and light 1 the moon (UE
// NUM_ATMOSPHERE_LIGHTS 2; SkyAtmosphere.usf:1117, 1403, 1688 add light 1 beside light 0). Each brings its
// own phase, its own transmittance down from the top of the atmosphere, its own planet shadow and its own
// multiple scattering, at its own outer-space illuminance -- so their in-scattering sources simply add.
// With no moon (no time of day, or the moon off) light 1's illuminance is zero and every LUT skips it on
// a uniform branch, so those levels pay nothing and draw what they drew.
bool SkyHasLight1() { return any(SkyLight1Illuminance.rgb > 0.0f); }

struct SkyLightPhase { float mie; float ray; };
// ParticipatingMediaCommon.ush:91-104; UE calls HG(g, -mu).
SkyLightPhase SkyPhaseFor(float3 lightDir, float3 viewDir)
{
    const float mu = dot(lightDir, viewDir), g = MieScattering.w;
    const float denom = max(1.e-6f, 1.0f + g*g - 2.0f*g*mu);
    SkyLightPhase phase;
    phase.mie = (1.0f-g*g) / (4.0f*SkyPi*denom*sqrt(denom));
    phase.ray = 3.0f*(1.0f+mu*mu) / (16.0f*SkyPi);
    return phase;
}
// The isotropic phase the distant sky light integrates with (UE usf:1393).
SkyLightPhase SkyIsotropicPhase()
{
    SkyLightPhase phase;
    phase.mie = 1.0f / (4.0f * SkyPi);
    phase.ray = 1.0f / (4.0f * SkyPi);
    return phase;
}

// One light's in-scattering source at a sample, AT ITS ILLUMINANCE: the single-scattered light (planet
// shadow, transmittance from the top of the atmosphere, phase) plus the multiple-scattering LUT's.
float3 SkyLightSource(float3 q, float height, float3 up, float3 lightDir, float3 illuminance, SkyLightPhase phase,
                      MediumSampleRGB medium, Texture2D<float4> transmittanceLut, Texture2D<float4> multiScatterLut,
                      SamplerState linearClamp)
{
    const float lightMu = dot(lightDir, up);
    float2 tUv;
    getTransmittanceLutUvs(height, lightMu, AtmosphereRadii.x, AtmosphereRadii.y, tUv);
    const float3 toLight = transmittanceLut.SampleLevel(linearClamp, tUv, 0).rgb;
    const float planet = SkyRaySphereNearest(q, lightDir, PlanetRadiusOffset * up, AtmosphereRadii.x);
    const float3 multi = multiScatterLut.SampleLevel(linearClamp,
        saturate(float2(lightMu*.5f+.5f, (height-AtmosphereRadii.x)/(AtmosphereRadii.y-AtmosphereRadii.x))), 0).rgb;
    return illuminance * ((planet >= 0 ? 0.0f : 1.0f) * toLight
        * (medium.ScatteringMie*phase.mie + medium.ScatteringRay*phase.ray) + multi*medium.Scattering);
}

// UE SkyAtmosphere.usf:590-606 and :1608: the in-scatter integral over ONE finite segment, exactly
// as the aerial perspective volume takes it -- `samples` uniform steps at offset .3, the sun's
// transmittance and the multi-scattering LUTs at every step, the planet shadow on the sun. Shared
// by the volume (sky_lut_aerial_cs.hlsl) and the cloud capture, which needs the air between a
// sea-level probe and a cloud front without a camera volume to read it from. `distanceScale` is
// UE's AerialPespectiveViewDistanceScale (1 when the caller has none): it multiplies the optical
// depth per sample and nothing else. Returns the luminance AT BOTH LIGHTS' ILLUMINANCE (N1: the sun and
// the moon summed inside the march; the caller applies the luminance factor and the exposure) and the
// RGB throughput.
void SkyIntegrateSegment(float3 p, float3 dir, float tMax, uint samples, float distanceScale,
                         Texture2D<float4> transmittanceLut, Texture2D<float4> multiScatterLut, SamplerState linearClamp,
                         out float3 L, out float3 throughput)
{
    float dt = tMax / samples;
    const SkyLightPhase phase0 = SkyPhaseFor(SkySunDirection.xyz, dir);
    const SkyLightPhase phase1 = SkyPhaseFor(SkyLight1Direction.xyz, dir);
    const bool light1 = SkyHasLight1();
    L = 0; throughput = 1;
    [loop] for (uint i = 0; i < samples; ++i)
    {
        float3 q = p + dir * ((i + 0.3f) * dt);
        float height = length(q);
        float3 up = q / height;
        MediumSampleRGB medium = SampleAtmosphereMediumRGB(q);
        // UE :601-606: the view-distance scale multiplies the optical depth per sample and nothing else
        // (the in-scatter step below keeps the unscaled extinction in its denominator, as UE do).
        float3 tr = exp(-medium.Extinction * dt * distanceScale);
        float3 S = SkyLightSource(q, height, up, SkySunDirection.xyz, SkyIlluminance.rgb, phase0, medium,
                                  transmittanceLut, multiScatterLut, linearClamp);
        [branch] if (light1)
        {
            S += SkyLightSource(q, height, up, SkyLight1Direction.xyz, SkyLight1Illuminance.rgb, phase1, medium,
                                transmittanceLut, multiScatterLut, linearClamp);
        }
        L += throughput * (S-S*tr) / max(medium.Extinction, 1.e-9f);
        throughput *= tr;
    }
}
#endif
#endif
