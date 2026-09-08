#pragma pack_matrix(row_major)
#define SKY_VIEW
#define SKY_AERIAL
#include "sky_atmosphere.hlsli"
#include "sky_aerial_common.hlsli"
#define SKY_AERIAL_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
Texture2D<float4> Transmittance : register(t0);
Texture2D<float4> MultiScatter : register(t1);
SamplerState LinearClamp : register(s0);
RWTexture3D<float4> AerialVolume : register(u0);

// UE SkyAtmosphere.usf:1466-1636. Delta: dispatch flattens Y/Z into Y (our 2D helper),
// world Y-up metres -> planet-local Z-up km, and start depth is the froxel FAR PLANE.
// Perspective main camera only; no reflection-capture, cloud/opaque shadows or ground bounce.
[numthreads(8, 8, 1)]
[RootSignature(SKY_AERIAL_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint3 cell = uint3(id.x, id.y % SkyAerialWidth, id.y / SkyAerialWidth);
    if (cell.x >= SkyAerialWidth || cell.z >= SkyAerialSlices) return;
    float2 uv = (cell.xy + 0.5f) / SkyAerialWidth;
    float4 view = mul(float4(uv * float2(2, -2) + float2(-1, 1), 1, 1), AerialInvProj);
    float3 viewDir = normalize(view.xyz / view.w);
    float3 dir = normalize(mul(float4(viewDir, 0), AerialInvView).xzy);
    float startKm = AerialStart.x / max(viewDir.z, 1.e-4f);
    float3 cam = float3(0, 0, SkyPlanet.x);
    float3 p = cam + startKm * dir;
    float tMax = pow((cell.z + 0.5f) / SkyAerialSlices, 2.0f) * SkyAerialDepthKm;
    float3 voxel = p + tMax * dir;
    float groundHit = SkyRaySphereNearest(cam, dir, 0, AtmosphereRadii.x);
    // No atmosphere segment beyond the start plane if the ground is already in front of it.
    if (groundHit >= 0.0f && groundHit <= startKm)
    { AerialVolume[cell] = float4(0, 0, 0, 1); return; }
    bool underGround = length(voxel) < AtmosphereRadii.x;
    bool belowHorizon = groundHit > 0.0f && startKm + tMax > groundHit;
    if (belowHorizon || underGround)
    {
        // UE :1527-1570: reproject hidden voxels to the ground/horizon to avoid a dark seam.
        cam += normalize(cam) * 0.02f;
        float3 voxelUp = normalize(voxel);
        float3 groundCam = normalize(cam) * AtmosphereRadii.x;
        float3 groundVoxel = voxelUp * AtmosphereRadii.x;
        if (belowHorizon && dot(normalize(cam - groundVoxel), voxelUp) < 0.0001f)
        {
            float3 middleGround = normalize(0.5f * (groundCam + groundVoxel)) * AtmosphereRadii.x;
            voxel = cam + 2.0f * (middleGround - cam);
        }
        else if (underGround) voxel = groundVoxel;
        dir = normalize(voxel - cam);
        p = cam + startKm * dir;
        tMax = max(0.0f, length(voxel - cam) - startKm);
    }
    // UE :1575-1601: clip finite segments for cameras outside the atmosphere.
    if (length(p) >= AtmosphereRadii.y)
    {
        float entry = SkyRaySphereNearest(p, dir, 0, AtmosphereRadii.y);
        if (entry < 0.0f || entry >= tMax)
        { AerialVolume[cell] = float4(0, 0, 0, 1); return; }
        p += entry * dir;
        p -= normalize(p) * PlanetRadiusOffset;
        tMax -= entry;
    }
    float bottom;
    tMax = min(tMax, SkyRayLength(p, dir, bottom));
    // UE :1608 and :590-604: fixed 2*(slice+1) samples, offset .3, uniform distance.
    uint samples = 2u * (cell.z + 1u);
    float dt = tMax / samples;
    float mu = dot(SkySunDirection.xyz, dir), g = MieScattering.w;
    float denom = max(1.e-6f, 1.0f + g*g - 2.0f*g*mu);
    float phaseMie = (1.0f-g*g) / (4.0f*SkyPi*denom*sqrt(denom));
    float phaseRay = 3.0f*(1.0f+mu*mu) / (16.0f*SkyPi);
    float3 L = 0, throughput = 1;
    [loop] for (uint i = 0; i < samples; ++i)
    {
        float3 q = p + dir * ((i + 0.3f) * dt);
        float height = length(q);
        float3 up = q / height;
        MediumSampleRGB medium = SampleAtmosphereMediumRGB(q);
        float3 tr = exp(-medium.Extinction * dt);
        float lightMu = dot(SkySunDirection.xyz, up);
        float2 tUv;
        getTransmittanceLutUvs(height, lightMu, AtmosphereRadii.x, AtmosphereRadii.y, tUv);
        float3 toLight = Transmittance.SampleLevel(LinearClamp, tUv, 0).rgb;
        float planet = SkyRaySphereNearest(q, SkySunDirection.xyz, PlanetRadiusOffset * up, AtmosphereRadii.x);
        float3 multi = MultiScatter.SampleLevel(LinearClamp,
            saturate(float2(lightMu*.5f+.5f, (height-AtmosphereRadii.x)/(AtmosphereRadii.y-AtmosphereRadii.x))), 0).rgb;
        float3 S = (planet >= 0 ? 0.0f : 1.0f) * toLight
            * (medium.ScatteringMie*phaseMie + medium.ScatteringRay*phaseRay) + multi*medium.Scattering;
        L += throughput * (S-S*tr) / max(medium.Extinction, 1.e-9f);
        throughput *= tr;
    }
    // UE :1618,1635-1636: luminance factor, pre-exposed RGB, mean RGB transmittance.
    AerialVolume[cell] = float4(min(L*SkyIlluminance.rgb*SkyIlluminance.w*SkyExposure.x, 64000.0f),
        saturate(dot(throughput, 1.0f/3.0f)));
}
