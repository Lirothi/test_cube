#include "sky_ibl_common.hlsli"
#include "ibl_common.hlsli"
#define SKY_IBL_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
cbuffer Filter : register(b0)
{
    uint size; uint mip; uint mipCount; uint diffuse;
    // rgb: the GROUND'S ALBEDO, a: how much of the ground replaces the sky below the horizon.
    // The diffuse probe is handed albedo 0, which reproduces UE's opaque black exactly -- see
    // SkyProbeSample for why that one must stay black.
    float4 groundAlbedoCoverage;
    // rgb: the sun's contribution to a level ground's outgoing radiance, E/PI, computed on the CPU
    // (outer-space illuminance x transmittance-toward-sun x cos, / PI). w: the Source mip that
    // holds the sky's sphere average, our stand-in for the sky's half of the same irradiance.
    float4 groundSunLight;
    // The SOURCE cube's shape, needed for the per-sample mip below. x: face size in texels,
    // y: mip count. Passed rather than assumed: `mipCount` above is the OUTPUT's, and the two are
    // only equal by today's coincidence (both 8) -- PrepareEnvironment sizes them independently.
    float4 sourceDims;
};
static const float sourceSize = sourceDims.x;
static const float sourceMipCount = sourceDims.y;
TextureCube<float4> Source : register(t0);
RWTexture2DArray<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);
// UE ReflectionEnvironmentShaders.usf:537-647, MonteCarlo.ush:58-63,248-261,347-363.
// Delta: source is a smooth, disk-free SkyView capture; sample mip 0, no source mip pyramid.
// Diffuse is E/PI in a cube (our existing consumers), rather than UE's SH coefficients.
//
// THESE ARE THE LIGHTING PROBES, and the lower-hemisphere policy is theirs. UE apply it to the
// captured cube (ReflectionEnvironmentShaders.usf:78-81 for the sky light capture, :205-211 for the
// downsample, :429-434 for the real-time one) with their own reason attached: "Assuming we're on a
// planet and no sky lighting is coming from below the horizon. This is important to avoid leaking
// from below since we are integrating incoming lighting and shadowing separately." It is a rule
// about an INTEGRAL OF INCOMING LIGHT, not about what the sky looks like from below.
//
// Our delta is only WHERE it is applied. UE have a single skylight cube and blacken it in place;
// we keep the captured radiance whole (the fog reads it as a picture -- see sky_ibl_capture_cs)
// and apply the same `lerp(sky, LowerHemisphereColor.rgb, LowerHemisphereColor.a)` here, per
// sample of the convolution and on the sharp mip-0 copy. The probes are bit-identical to a
// blackened capture; the difference is that the picture survives for the consumer that needs one.
// THE LOWER HEMISPHERE IS THE GROUND, NOT BLACK -- our one deliberate departure from UE here.
//
// UE replace it with an opaque black and their reason (quoted above) is about not double-counting:
// the ground's light comes back through geometry, reflection captures and Lumen. That holds for
// THEM. Here it did not, and the owner found it as "the sphere is covered in blotches when the
// procedural sky recomputes environment lighting": a metallic ball at roughness 0.08 reads this
// probe almost directly, its lower half reflects downwards, and downwards was black. Measured on
// the equirect dumps (compose debug views 8/9): the radiance capture is symmetric about the horizon
// to a tenth of a code, while the probe read 204 at +27 degrees and exactly 0.0 at -27.
//
// `lighting_cs.hlsl` already had the other half of this written down, as a known asymmetry:
//   "DIFFUSE ONLY. A down-facing glossy surface really does reflect bright sand, but the prefiltered
//    sky cube has no such term either, and adding one on this side only would make the two disagree."
// So the two now agree the other way -- both carry the ground.
//
// THE DIFFUSE PROBE MUST STAY BLACK, and that is why the albedo arrives per dispatch rather than
// being a constant here: `lighting_cs.hlsl:336` is `irradiance * skyIrradianceScale +
// GroundBounceOverPi(N)`, so the diffuse path already adds the ground itself. Putting it in that
// cube too would count it twice. The CPU passes albedo 0 for the diffuse pass, which collapses the
// expression below to UE's black exactly.
float3 SkyProbeSample(float3 dir, float sourceMip)
{
    const float3 sky = Source.SampleLevel(LinearClamp, dir, sourceMip).rgb;
    // World Y-up here: `dir` comes from SkyCubeDirection / the basis built on it, unswizzled.
    if (dir.y >= 0.0f) { return sky; }
    // The sky's half of the ground's irradiance. The capture MIRRORS the sky through the horizon,
    // so the cube's sphere average is the hemisphere average -- one tap instead of a second
    // convolution, and it costs a texel of the coarsest mip.
    const float3 skyOnGroundOverPi =
        Source.SampleLevel(LinearClamp, float3(0.0f, 1.0f, 0.0f), groundSunLight.w).rgb;
    const float3 ground = groundAlbedoCoverage.rgb * (groundSunLight.rgb + skyOnGroundOverPi);
    return lerp(sky, ground, saturate(groundAlbedoCoverage.a));
}
[numthreads(8, 8, 1)]
[RootSignature(SKY_IBL_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint face = id.y / size;
    uint2 pixel = uint2(id.x, id.y % size);
    if (pixel.x >= size || face >= 6) return;
    float3 N = SkyCubeDirection((pixel + 0.5f) / size, face);
    float roughness = saturate(IblRoughnessFromMip(mip, mipCount));
    if (!diffuse && mip == 0)
    {
        Output[uint3(pixel, face)] = float4(SkyProbeSample(N, 0.0f), 1);
        return;
    }
    // Orthonormal basis in our world axes; isotropic integral is independent of its rotation.
    float3 T = normalize(cross(abs(N.z) < 0.999f ? float3(0,0,1) : float3(1,0,0), N));
    float3 B = cross(N, T);
    const uint samples = 64;
    // THE SOURCE MIP PYRAMID, which the header above used to list as our delta from UE and which is
    // what makes 64 samples enough. Their ReflectionEnvironmentShaders.usf picks a source mip from
    // each sample's SOLID ANGLE, so a sparse estimate reads a pre-blurred source instead of point-
    // sampling it. It matters here more than it does for them, because `SkyProbeSample` has a HARD
    // STEP at the horizon (sky above, ground below): a step edge is exactly the integrand a
    // low-discrepancy set handles worst, and how many of the 64 samples land on each side varies
    // with the texel's normal -- which is the blotching the owner saw on a metallic-0 surface, where
    // the 32^2 irradiance probe is the whole answer.
    //
    // MEASURED before choosing this over brute force: raising the count to 1024 took the sphere's
    // large-scale variation from 4.35 to 3.33 against an HDRI reference of 3.26 -- i.e. it IS the
    // convolution -- but cost 7.4 ms per environment rebuild, which is a hitch on every drag of the
    // sun slider. The pyramid buys the same smoothness with the same 64 taps, and better locality.
    //
    // `SolidAngleTexel` is the source cube's, not the output's: 4*PI over its texel count.
    const float kPi = 3.14159265358979323846f;
    const float solidAngleTexel = 4.0f * kPi / (6.0f * sourceSize * sourceSize);
    const float maxSourceMip = max(sourceMipCount - 1.0f, 0.0f);
    float3 sum = 0;
    float weight = 0;
    [loop] for (uint i = 0; i < samples; ++i)
    {
        float2 E = float2(float(i) / samples, float(reversebits(i)) * 2.3283064365386963e-10f);
        float phi = 2.0f * 3.14159265358979323846f * E.x;
        bool cosine = diffuse != 0 || roughness > 0.99f;
        E.y *= cosine ? 1.0f : 0.995f;
        float a2 = pow(roughness, 4.0f);
        float c = cosine ? sqrt(E.y) : sqrt((1.0f - E.y) / (1.0f + (a2 - 1.0f) * E.y));
        float s = sqrt(max(0.0f, 1.0f - c*c));
        float3 H = float3(s*cos(phi), s*sin(phi), c);
        float3 L = cosine ? H : 2.0f * H.z * H - float3(0,0,1);
        if (L.z > 0 || cosine)
        {
            float w = cosine ? 1.0f : L.z;
            // The density this sample was drawn from. Cosine: c/PI. GGX: D(H)/4, the same form UE
            // use for their specular mip (the NdotH / (4 VdotH) factors cancel for an isotropic
            // lobe sampled about N).
            const float denom = H.z * H.z * (a2 - 1.0f) + 1.0f;
            const float pdf = cosine ? max(c, 1.0e-4f) / kPi
                                     : 0.25f * a2 / max(kPi * denom * denom, 1.0e-8f);
            const float solidAngleSample = 1.0f / max((float)samples * pdf, 1.0e-8f);
            const float sampleMip =
                clamp(0.5f * log2(solidAngleSample / solidAngleTexel), 0.0f, maxSourceMip);
            sum += SkyProbeSample(L.x*T + L.y*B + L.z*N, sampleMip) * w;
            weight += w;
        }
    }
    Output[uint3(pixel, face)] = float4(max(sum / max(weight, 1.e-6f), 0.0f), 1);
}
