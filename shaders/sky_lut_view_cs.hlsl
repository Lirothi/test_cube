#define SKY_VIEW
#include "sky_atmosphere.hlsli"
#include "sky_view_mapping.hlsli"
#define SKY_VIEW_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
Texture2D<float4> Transmittance : register(t0);
Texture2D<float4> MultiScatter : register(t1);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> SkyView : register(u0);

float3 ViewTransmittance(float height, float mu)
{
    float2 uv;
    getTransmittanceLutUvs(height, mu, AtmosphereRadii.x, AtmosphereRadii.y, uv);
    return Transmittance.SampleLevel(LinearClamp, uv, 0).rgb;
}
// UE SkyAtmosphere.usf:1282-1338,519-758. One sun, no opaque/cloud shadows.
// Pre-exposed radiance like UE: prevents the solar disk overflowing our FP16 light target.
[numthreads(8, 8, 1)]
[RootSignature(SKY_VIEW_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(192, 104))) return;
    float3 dir = SkyViewUvToDir((id.xy + 0.5f) / SkyViewSize, SkyPlanet.x, AtmosphereRadii.x);
    float3 p = float3(0, 0, SkyPlanet.x);
    // UE MoveToTopAtmosphere, usf:166-187: permit cameras above the atmosphere.
    if (SkyPlanet.x >= AtmosphereRadii.y)
    {
        float entry = SkyRaySphereNearest(p, dir, 0, AtmosphereRadii.y);
        if (entry < 0) { SkyView[id.xy] = float4(0, 0, 0, 1); return; }
        p += entry * dir;
        p -= normalize(p) * PlanetRadiusOffset;
    }
    float bottom;
    float tMax = SkyRayLength(p, dir, bottom);
    float samples = lerp(4.0f, 32.0f, saturate(tMax / 150.0f));
    float countFloor = floor(samples);
    float tMaxFloor = tMax * countFloor / samples;
    float mu = dot(SkySunDirection.xyz, dir);
    // ParticipatingMediaCommon.ush:91-104; UE calls HG(g, -mu).
    float g = MieScattering.w;
    float denom = max(1.e-6f, 1.0f + g * g - 2.0f * g * mu);
    float phaseMie = (1.0f - g * g) / (4.0f * SkyPi * denom * sqrt(denom));
    float phaseRay = 3.0f * (1.0f + mu * mu) / (16.0f * SkyPi);
    float3 L = 0, throughput = 1;
    [loop] for (float i = 0; i < samples; i += 1)
    {
        float t0 = pow(i / countFloor, 2.0f) * tMaxFloor;
        float t1 = pow((i + 1.0f) / countFloor, 2.0f);
        t1 = t1 > 1.0f ? tMax : t1 * tMaxFloor;
        float dt = t1 - t0;
        float3 q = p + dir * (t0 + dt * 0.3f);
        float height = length(q);
        float3 up = q / height;
        MediumSampleRGB m = SampleAtmosphereMediumRGB(q);
        float3 tr = exp(-m.Extinction * dt);
        float lightMu = dot(SkySunDirection.xyz, up);
        float planet = SkyRaySphereNearest(q, SkySunDirection.xyz, PlanetRadiusOffset * up, AtmosphereRadii.x);
        float3 multi = MultiScatter.SampleLevel(LinearClamp,
            saturate(float2(lightMu * 0.5f + 0.5f, (height - AtmosphereRadii.x) / (AtmosphereRadii.y - AtmosphereRadii.x))), 0).rgb;
        float3 S = (planet >= 0 ? 0.0f : 1.0f) * ViewTransmittance(height, lightMu)
            * (m.ScatteringMie * phaseMie + m.ScatteringRay * phaseRay) + multi * m.Scattering;
        L += throughput * (S - S * tr) / max(m.Extinction, 1.e-9f);
        throughput *= tr;
    }
    // NO GROUND TERM HERE. UE render this LUT with `const bool Ground = false`
    // (SkyAtmosphere.usf:1325, RenderSkyViewLutCS), and that is not an optimisation -- it is what
    // keeps the LUT CONTINUOUS across its own horizon.
    //
    // The LUT's V axis is split AT the horizon: v < 0.5 is the sky, v > 0.5 is below it
    // (SkyViewLutParamsToUv, transcribed in sky_view_mapping.hlsli). The two halves are separate
    // parameterisations that meet at v = 0.5, so whatever the integral returns there has to agree
    // from both sides or the seam becomes a LINE ACROSS THE SKY. Pure scattering agrees: a ray
    // grazing the horizon and one just below it travel almost the same air. A Lambert-lit planet
    // surface does not -- it appears at full brightness the instant the ray intersects, and B2 put
    // it here deliberately ("Plan B2 wants a ground hemisphere"). The owner found the line by
    // climbing to 377 m, where it is unmistakable; it was always there, and it shows on any
    // near-horizontal direction with or without the ocean in front of it.
    //
    // Nothing is lost by removing it. The ground's contribution to LIGHTING is owned elsewhere and
    // was already separated in B6.0: `light.groundAlbedo` drives GroundBounceOverPi in deferred and
    // RT hit shading, and the multi-scattering LUT keeps its own ground term exactly as UE do
    // (they pass Ground=true there). What disappears is only a painted plate in the camera's sky --
    // the thing UE expect real geometry to cover.
    SkyView[id.xy] = float4(min(L * SkyIlluminance.rgb * SkyIlluminance.w * SkyExposure.x, 64000.0f), dot(throughput, 1.0f / 3.0f));
}
