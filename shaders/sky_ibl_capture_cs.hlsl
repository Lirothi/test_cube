#include "sky_view_mapping.hlsli"
#include "sky_ibl_common.hlsli"
#include "night_sky.hlsli"
#include "ibl_common.hlsli"
#include "height_fog.hlsli"
#include "uds_sky.hlsli"
#define SKY_IBL_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
cbuffer Capture : register(b0)
{
    float4 planet;     // height, bottom, 1/fixed preExposure, target mip size
    float4 captureMip; // x: supersample count per axis, y: the atmosphere's top radius (km), z: the Milky Way map's mip
    // Time-of-day plan N2 (NightSkyFrameData): the night sky's own light, in the picture as in the sky
    // pixel -- the airglow and the Milky Way, and the stars as their mean (a 0.7-degree texel cannot
    // hold a point). nightRow0.w 0 = none.
    float4 nightGlow;
    float4 nightLayers;
    float4 nightRow0;
    float4 nightRow1;
    float4 nightRow2;
    // UE's DistantHeightFog (ReflectionEnvironmentRealTimeCapture.cpp:919-962, ReflectionEnvironmentShaders.usf:1041-1073;
    // the fog's bVisibleInRealTimeSkyCaptures, default true): the analytic height fog at an infinite distance from the
    // probe, `sky x T + FogColor`, over the sky and before the clouds. PackHeightFog's numbers as compose takes them
    // (fogParams0.x 0 or fogInscatter.w 0 = none: the HDRI sky, the sky-along-view A/B, `fog.inSkyCapture:0`); the sun's
    // colour on the ground and the direction TO it (the lobe); fogSunDir.w = the probe's world height (sea level, 0).
    float4 fogParams0;
    float4 fogParams1;
    float4 fogParams2;
    float4 fogInscatter;
    float4 fogSunColor;
    float4 fogSunDir;
    float4 fogSunDirectional; // the lobe's own coloured term (DirectionalInscatteringLuminance) / 4 pi
    // Ultra Dynamic Sky's sky material over the atmosphere (UdsSkyFrameData, uds_sky.hlsli): UDS's sky light captures its
    // sky sphere, so its probes see the layer -- the glow x 2, its Night Sky Glow Reflection Intensity. udsGlow.w 0 = off.
    float4 udsParams;
    float4 udsGlow;
    float4 udsSun;
    float4 udsMoon;
};
Texture2D<float4> SkyView : register(t0);
Texture2D<float4> Transmittance : register(t1); // N2: the air in front of the night sky
Texture2D<float4> NightMap : register(t2);      // N2: the Milky Way (night_sky.hlsli units)
RWTexture2DArray<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);
// UE SkyAtmosphere.usf:956-974; :314-317 omits the sun disk during capture to avoid
// double specular (the directional light already supplies it). Fixed sea-level probe.
//
// THIS CUBE IS A PICTURE OF THE SKY, NOT A LIGHTING PROBE. It is the whole sphere, ground term
// included, and it is the source the FOG samples along the view ray -- the direct analogue of UE's
// `InscatteringColorCubemap` on the height fog component ("useful to make distant, heavily fogged
// scene elements match the sky", ExponentialHeightFogComponent.h:50), which is likewise a full
// 4pi asset with no horizon policy on it.
//
// The lower-hemisphere policy that UE's SKYLIGHT capture carries (ReflectionEnvironmentShaders.usf
// :78-81, :205-211, :412-434 -- "no sky lighting is coming from below the horizon ... to avoid
// leaking from below since we are integrating incoming lighting and shadowing separately") belongs
// to the lighting probes, and lives in sky_ibl_filter_cs.hlsl, which builds them from this cube.
// Applying it HERE made the one texture serve both roles, and the fog -- whose view ray points
// DOWN at every pixel of ground and water it runs on -- read the policy value as if it were sky.
//
// The mip chain is UE's too: their fog blends the top mip (non-directional, the sphere average)
// into mip 0 (directional) by distance, so a short, steeply-downward ray never resolves a
// direction. Each mip is captured straight from the SkyView LUT with `captureMip.x` supersamples
// per axis rather than by downsampling the level above, so no mip depends on another and the whole
// chain is one barrier-free run of dispatches.
[numthreads(8, 8, 1)]
[RootSignature(SKY_IBL_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint size = (uint)planet.w, face = id.y / size;
    uint2 pixel = uint2(id.x, id.y % size);
    if (pixel.x >= size || face >= 6) return;
    const uint taps = max((uint)captureMip.x, 1u);
    const float invTaps = 1.0f / (float)taps;
    float3 sum = 0.0f.xxx;
    // The fog's numbers once (compose_cs.hlsl's sky branch, UE's colour: the volume is not in a capture, UE's ExcludeDistance 0).
    const bool fogOn = fogParams0.x > 0.0f && fogInscatter.w > 0.5f;
    HeightFogParams fog;
    fog.density = fogParams0.x;
    fog.heightFalloff = fogParams0.y;
    fog.referenceHeight = fogParams0.z;
    fog.startDistance = fogParams0.w;
    fog.maxOpacity = fogParams1.x;
    fog.sunGroundScale = fogParams1.y;
    fog.sunDirectional = fogSunDirectional.rgb;
    fog.sunScatterExponent = fogParams1.z;
    fog.sunScatterStartDistance = fogParams1.w;
    for (uint sy = 0; sy < taps; ++sy)
    {
        for (uint sx = 0; sx < taps; ++sx)
        {
            const float2 uv = (pixel + (float2(sx, sy) + 0.5f) * invTaps) / size;
            float3 dir = SkyCubeDirection(uv, face).xzy; // local Z-up for the SkyView mapping
            // SAME LOWER HEMISPHERE AS THE DRAWN SKY. skybox.hlsl clamps everything at or below the
            // horizon to the horizon so the planet's curvature never shows in the sky; this cube is
            // a PICTURE of that same sky -- the fog samples it along the view ray, which points DOWN
            // on every pixel of ground and water -- so it must be clamped identically, or the two
            // disagree exactly where they meet.
            //
            // Without this the chain bites twice over: the SkyView LUT no longer paints a planet
            // surface (UE's Ground=false), so its below-horizon half is only the shortened
            // scattering integral, which is nearly black. Captured verbatim that darkness becomes
            // the fog's sky colour for every downward ray and puts a black band back on the far
            // water -- the very artefact B6.0 removed, arriving by a different road.
            // The SAME clamp the drawn sky uses (sky_view_mapping.hlsli), not merely `ground=false`
            // and not a clamp on the direction: all three readers have to land on one texel or they
            // disagree at the only row where they meet.
            float3 tap = SkyView.SampleLevel(LinearClamp, SkyViewDirToUvNoPlanet(dir, planet.x, planet.y), 0).rgb;
            // Ultra Dynamic Sky's sky material on the atmosphere (raw radiance: the tap is stored x 1 / planet.z).
            [branch] if (udsGlow.w > 0.0f)
            {
                tap = UdsSkyLayer(tap * planet.z, SkyCubeDirection(uv, face), udsParams, udsGlow, udsSun, udsMoon, 2.0f) / max(planet.z, 1.e-8f);
            }
            // N2: what the night sky adds. The airglow above the horizon and held at its horizon value
            // below it, as the drawn sky is; the Milky Way and the stars' mean above it only.
            [branch] if (nightRow0.w > 0.0f)
            {
                const float3 world = SkyCubeDirection(uv, face);
                const float up = max(dir.z, 0.0f);
                const float bottom = planet.y, top = captureMip.y, height = planet.x;
                const float H = sqrt(top * top - bottom * bottom);
                const float rho = sqrt(max(0.0f, height * height - bottom * bottom));
                const float d = max(0.0f, -height * up + sqrt(max(0.0f, height * height * (up * up - 1.0f) + top * top)));
                const float3 tr = Transmittance.SampleLevel(LinearClamp, float2((d - (top - height)) / max(1.e-6f, rho + H - (top - height)), rho / H), 0).rgb;
                float3 night = nightGlow.rgb * NightAirglowVanRhijn(up);
                if (dir.z > 0.0f)
                {
                    const float3 eq = float3(dot(nightRow0.xyz, world), dot(nightRow1.xyz, world), dot(nightRow2.xyz, world));
                    night += NightMap.SampleLevel(LinearClamp, NightMapUv(eq), captureMip.z).rgb * (kNightMapUnit * nightLayers.y);
                    night += kNightStarsMeanRadiance * nightLayers.x;
                }
                // The picture is stored like the LUT, pre-exposed by 1 / planet.z, and decoded below.
                tap += night * tr / max(planet.z, 1.e-8f);
            }
            [branch] if (fogOn)
            {
                // At "infinity" as compose fogs a sky pixel: kSkyFogDistance, cut where the height term has reached UE's
                // exp2 clamp (a steep ray changes nothing past it). Raw radiance (cd/m2), so the LUT's tap is decoded first.
                const float3 world = SkyCubeDirection(uv, face);
                const float reach = min(1.0e5f, 127.0f / (max(fog.heightFalloff, 1.0e-4f) * max(abs(world.y), 1.0e-6f)));
                const float pointHeight = fogSunDir.w + world.y * reach;
                const float fogShared = HeightFogSharedIntegral(reach, fogSunDir.w, pointHeight, fog);
                const float tau = HeightFogOpticalDepth(fogShared, reach, fog);
                const float T = HeightFogTransmittance(tau, HeightFogMinTransmittance(tau, fog.maxOpacity));
                const float3 air = HeightFogColor(fogInscatter.rgb, fogSunColor.rgb, dot(world, fogSunDir.xyz), fogShared, reach, T, fog);
                tap = (tap * planet.z * T + air) / max(planet.z, 1.e-8f);
            }
            sum += tap;
        }
    }
    const float3 L = sum * (invTaps * invTaps);
    // Persistent RAW radiance: camera exposure must not dirty or rescale the environment.
    // NOT scaled by the sky-only luminance factor, deliberately. UE's capture renders their sky pass
    // and so carries SkyLuminanceFactor; ours is a PICTURE knob and must leave the lighting alone --
    // measured 2026-09-11: with the capture scaled, "sky-only" 2 moved lit sand x1.11 and palm crowns
    // x1.37, exactly what the all-LUT `luminanceScale` did, and the two knobs were one knob.
    Output[uint3(pixel, face)] = float4(min(max(L * planet.z, 0.0f), 65504.0f), 1);
}
