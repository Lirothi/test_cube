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
    // NO EARLY-OUT HERE. An earlier version zeroed the voxel when the ground was already in front of
    // the start plane; UE have no such branch -- SkyAtmosphere.usf:1517-1571 handle a ray that is
    // already through the ground purely by reprojection, so every voxel gets a value. Ours killed the
    // ENTIRE depth column for the affected direction, and the volume is only 32 texels across, so the
    // step against the neighbouring band that kept its aerial perspective was smeared over a wide
    // vertical stripe -- the streaking that showed up wherever the far field went featureless.
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
    // UE :1608 and :590-604: fixed 2*(slice+1) samples, offset .3, uniform distance. The integral
    // itself is SkyIntegrateSegment (sky_atmosphere.hlsli), shared with the cloud capture (C4).
    uint samples = 2u * (cell.z + 1u);
    float3 L, throughput;
    SkyIntegrateSegment(p, dir, tMax, samples, AerialStart.y, Transmittance, MultiScatter, LinearClamp, L, throughput);
    // UE :1618,1635-1636: luminance factor, pre-exposed RGB, mean RGB transmittance.
    AerialVolume[cell] = float4(min(L*SkyIlluminance.rgb*SkyIlluminance.w*SkyExposure.x, 64000.0f),
        saturate(dot(throughput, 1.0f/3.0f)));
}
