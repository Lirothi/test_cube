// TIME-OF-DAY PLAN N2: THE MILKY WAY, generated once into an equirectangular map in equatorial
// coordinates (night_sky.hlsli NightMapUv), radiance in kNightMapUnit (1e-4 cd/m2) before the air.
// Procedural (the owner's decision): a band along the real galactic plane, thicker and brighter toward
// the centre in Sagittarius, the bulge, clumped star clouds and the dark dust lanes of the Great Rift
// from Cygnus down to Sagittarius. Every mip is computed straight from the model at its own resolution,
// the octaves it cannot hold left out, so no mip reads another (sky_moon_cs.hlsl's pattern).
//
// Brightness: the brightest star clouds near the centre ~20.5 mag/arcsec2 (~6e-4 cd/m2), the band
// elsewhere ~21.5-22 (1-2.5e-4) -- the same order as the dark sky's own airglow, which is why the Milky
// Way shows only far from towns and only once the eye has adapted to it.

#include "night_sky.hlsli"

#define SKY_NIGHT_RS "CBV(b0), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer NightMapCB : register(b0)
{
    uint mapWidth;
    uint mapHeight;
    uint octaves; // fBm octaves this mip can hold
    uint seed;
};

RWTexture2D<float3> NightMap : register(u0);

float3 MilkyWay(float3 eq)
{
    const float b = degrees(asin(clamp(dot(eq, kGalacticPole), -1.0f, 1.0f)));
    const float l = degrees(atan2(dot(eq, kGalactic90), dot(eq, kGalacticCentre)));

    // The disc: a thin band, fattening and brightening toward the centre.
    const float thickness = 4.5f + 4.0f * exp(-pow(l / 45.0f, 2.0f));
    const float disc = (0.35f + 0.65f * exp(-pow(l / 75.0f, 2.0f))) * exp(-pow(b / thickness, 2.0f));
    // The bulge round the centre, a little flattened.
    const float bulge = 1.6f * exp(-(pow(l / 13.0f, 2.0f) + pow(b / 9.0f, 2.0f)));

    // Star clouds: the band is clumped, not smooth.
    const float clouds = NightFbm(eq * 5.0f, octaves, seed);
    const float fine = NightFbm(eq * 17.0f + 3.1f, octaves, seed + 101u);
    // Dust: filaments along the plane, densest in the Great Rift (l from about -10 to 80, just off b = 0).
    const float filaments = smoothstep(0.42f, 0.68f, NightFbm(eq * 9.0f + 7.7f, octaves, seed + 211u));
    const float riftCentre = 1.2f * sin(radians(l * 2.0f));
    const float rift = exp(-pow((b - riftCentre) / 2.6f, 2.0f)) * smoothstep(100.0f, 30.0f, abs(l - 35.0f));
    const float dust = saturate(0.85f * rift * (0.35f + 0.65f * filaments) + 0.45f * filaments * exp(-pow(b / 7.0f, 2.0f)));

    const float glow = (disc * (0.45f + 1.1f * clouds) * (0.8f + 0.4f * fine) + bulge * (0.6f + 0.8f * clouds)) * (1.0f - dust);
    // Colour: the bulge's old stars warm, the disc whiter; dust reddens what it lets through.
    const float bulgeShare = saturate(bulge / max(disc + bulge, 1.0e-4f));
    float3 colour = lerp(float3(0.96f, 0.98f, 1.04f), float3(1.08f, 0.98f, 0.82f), bulgeShare);
    colour = lerp(colour, colour * float3(1.10f, 0.95f, 0.78f), dust);
    // 1e-4 units: the band ~1-2.5, the centre's star clouds ~5.
    return colour * glow * 2.4f;
}

[numthreads(8, 8, 1)]
[RootSignature(SKY_NIGHT_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= mapWidth || id.y >= mapHeight) return;
    const float2 uv = (float2(id.xy) + 0.5f) / float2(mapWidth, mapHeight);
    NightMap[id.xy] = MilkyWay(NightMapDir(uv));
}
