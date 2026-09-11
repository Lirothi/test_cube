#define SKYBOX_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "sky_view_mapping.hlsli"

cbuffer PerFrame : register(b0)
{
    float4x4 view;      // regular view matrix
    float4x4 proj;
    float4x4 prevView;
    float4x4 prevProj;
    float4x4 projNoJitter;
    float4x4 prevProjNoJitter;
    float exposure;
    float4 skySunDirection;
    float4 skyIlluminance;
    float4 skyExposure;
    float4 skyPlanet;
}

struct VSIn {
    float3 pos : POSITION;
};

struct VSOut {
    float4 pos : SV_Position;
    float4 prevPos : TEXCOORD1;
    float3 dir : TEXCOORD0; // sampling direction
    float4 clipPos : TEXCOORD2;
};

[RootSignature(SKYBOX_RS)]
VSOut VSMain(VSIn i)
{
    // 1) Position: remove translation from view, apply proj, and push to the far plane
    float4x4 v = view;
    v._41 = 0.0;
    v._42 = 0.0;
    v._43 = 0.0; // remove translation (HLSL uses _41.._43)
    VSOut o;
    float4 viewPos = mul(float4(i.pos, 1.0), v);
    o.pos = mul(viewPos, proj);
    o.pos.z = 0.0f;
    float4 clipNoJitter = mul(viewPos, projNoJitter);
    clipNoJitter.z = 0.0f;
    o.clipPos = clipNoJitter;

    float4x4 pv = prevView;
    pv._41 = 0.0;
    pv._42 = 0.0;
    pv._43 = 0.0;
    float4 prevViewPos = mul(float4(i.pos, 1.0), pv);
    o.prevPos = mul(prevViewPos, prevProjNoJitter);
    o.prevPos.z = 0.0f;

    //float3 dirWS = mul(viewPos.xyz, (float3x3) invView).xyz;
    float3 dirWS = i.pos;
    o.dir = normalize(dirWS);
    
    return o;
}

TextureCube sky : register(t0);
Texture2D<float4> skyViewLut : register(t1);
Texture2D<float4> transmittanceLut : register(t2);
SamplerState samLinear : register(s0);

struct PSOut
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
};

[RootSignature(SKYBOX_RS)]
PSOut PSMain(VSOut i)
{
    float3 c = sky.Sample(samLinear, i.dir).rgb * exposure;
    if (skyPlanet.w != 0.0f)
    {
        float3 dir = normalize(i.dir.xzy);
        float height = skyPlanet.x, bottom = skyPlanet.y, top = skyPlanet.z;
        // CUBEMAP-LIKE LOWER HEMISPHERE. Everything at or below the horizon reads the HORIZON, and
        // the planet's curvature stops being visible in the drawn sky.
        //
        // The LUT is physical and stays that way: its V axis splits at the horizon, whose angle
        // comes from the view height, so the split DIPS as the camera climbs (0.03 deg at 1 m,
        // 0.62 deg at 377 m) and everything below it darkens as the ray meets the planet sooner.
        // Both are correct for a planet and both are visible as soon as real geometry stops short
        // of the horizon -- which the ocean always does. A plain HDRI cube has neither: its equator
        // sits at eye level at any altitude and below it is simply more sky.
        //
        // So clamp the DRAWN sky to the upper half. `ground` is forced false and the direction is
        // flattened onto the horizon, which makes the sample continuous by construction: the last
        // value above the horizon is also every value below it. Delta from UE, deliberately: they
        // let the planet show because their worlds fill the lower hemisphere with terrain.
        //
        // The LUT itself is untouched, so the aerial-perspective volume, the IBL capture and the
        // distant sky-light LUT keep the real atmosphere. This is a DRAW-side choice only.
        // Whether the ray actually meets the planet. This is NOT used to pick a LUT branch any more
        // -- the sky is mirrored across the horizon by SkyViewDirToUvNoPlanet -- but the SUN DISC
        // still needs it, and it is the disc's only planet test. Folding it away into a constant
        // `false` (which the first version of this clamp did) left the disc drawn below the horizon
        // at saturated radiance, with its transmittance pinned to the LUT's clamped edge texel so it
        // never dimmed at any negative sun elevation. UE guard it twice, SkyAtmosphere.usf:315-317
        // and SkyAtmosphereCommon.ush:241-245.
        const bool ground = SkyViewIntersectsGround(dir, height, bottom);
        // The LUT is pre-exposed for storage; LightTarget is RAW radiance for HDRI
        // and surface lighting alike. Decode here so compose uses one exposure path.
        // skyExposure.y = the sky PICTURE's own brightness, applied at draw time and NOWHERE else: not the
        // capture the probes are built from, not the distant light, not the disc. UE's SkyLuminanceFactor
        // (SkyAtmosphere.usf:919-922) also reaches their sky-light capture; ours deliberately does not,
        // so this knob brightens the sky you see and leaves what the sky lights alone. The LUT already
        // carries SkyAndAerialPerspectiveLuminanceFactor (`luminanceScale`), which is the one that lights.
        c = skyViewLut.SampleLevel(samLinear, SkyViewDirToUvNoPlanet(dir, height, bottom), 0).rgb
            / max(skyExposure.x, 1.e-8f) * skyExposure.y;
        // UE SkyAtmosphereCommon.ush:256-279, Rendering.cpp:445-446.
        // skySunDirection.w = the sun's HALF-APEX angle in radians (DirectionalLight::GetSunHalfApexRadians,
        // UE GetSunLightHalfApexAngleRadian): the level sun's LightSourceAngle, 0.5357 deg apex by default.
        float radius = skySunDirection.w;
        float cosHalf = cos(radius);
        float viewDotLight = dot(dir, skySunDirection.xyz);
        if (!ground && radius > 0.0f && viewDotLight > cosHalf)
        {
            float H = sqrt(top * top - bottom * bottom);
            float h = min(height, top);
            float rho = sqrt(max(0.0f, h * h - bottom * bottom));
            float d = max(0.0f, -h * dir.z + sqrt(max(0.0f, h*h*(dir.z*dir.z-1.0f)+top*top)));
            float2 uv = float2((d - (top-h)) / max(1.e-6f, rho+H-(top-h)), rho/H);
            float3 tr = height >= top ? 1.0f : transmittanceLut.SampleLevel(samLinear, uv, 0).rgb;
            // 2*sin(radius/2)^2 avoids cancellation for very small disks.
            float oneMinusCos = 2.0f * pow(sin(0.5f * radius), 2.0f);
            float softEdge = saturate(2.0f * (viewDotLight - cosHalf) / max(oneMinusCos, 1.e-12f));
            // UE applies SkyAndAerialPerspectiveLuminanceFactor to scattering, not the disk.
            // The FP16 radiance range is matched BEFORE exposure: a procedural-only pre-exposure
            // bypass gave bloom thousands of times more energy than an equally bright HDRI sun or
            // specular highlight. Coverage stays outside the limit so the soft edge is antialiased.
            // HOW MUCH OF THE TRANSMITTANCE THE DISC SHOWS IS A LOOK CONSTANT, and it is one only
            // because the FP16 ceiling took the physics away. UE clamp the disc nowhere -- not in
            // the shader (`SkyAtmosphereCommon.ush:271` returns transmittance * luminance * softEdge
            // raw) and not on the CPU (`SkyAtmosphereRendering.cpp:444-446`) -- because they
            // pre-expose, so the value lands near the display's white point and per-channel clipping
            // does the rest: white core, coloured falloff. We store RAW radiance in FP16, and at this
            // project's illuminance the disc is about 7e7 against a ceiling of 65504.
            //
            // A thousand times over the ceiling leaves no middle ground. Clamping per channel pins
            // all three and the disc is pure white at every sun angle -- which is what it did, and is
            // why it never reddened at sunset. Clamping by the peak keeps the hue but applies it to
            // the WHOLE disc at once, since the peak scale is uniform, so it reads as a coloured
            // plate rather than an over-exposed highlight -- far too yellow. Moving the clamp after
            // `softEdge` does not help either: the core is still a thousand times over, so it clips
            // to white and the tinted rim is a sliver a fraction of a pixel wide.
            //
            // So the amount is chosen, and named. The proper fix is to pre-expose this path as UE do,
            // at which point this constant goes away and the clipping decides again.
            //
            // WHAT THIS CONSTANT MAY AND MAY NOT DECIDE. Only the HUE, because the peak clamp two
            // lines down erases everything else: the disc arrives about a thousand times over the
            // ceiling, so `disk * (65504 / diskPeak)` renormalises it and any scale applied here
            // cancels exactly. Writing the knob as if it also controlled brightness would be a
            // control that lies.
            //
            // IT IS NOT `lerp(1, tr, k)` ANY MORE, and that shape was backwards. A lerp towards
            // white leaves a floor of (1 - k) white under every channel, so the redder the true sun
            // gets the more that floor dominates -- the coloured part shrinks towards zero while the
            // white part stays put. Measured on this level's parameters (rayleigh x0.70), the hue it
            // produced ran 1.00 : 0.90 : 0.82 at 7.8 deg of elevation and 1.00 : 0.93 : 0.91 at the
            // horizon: the disc went WHITER as it set, while the physical transmittance went
            // 1.00 : 0.63 : 0.31 -> 1.00 : 0.16 : 0.00 the other way.
            //
            // An exponent on the peak-normalised transmittance keeps the hue's DIRECTION and only
            // shortens how far it travels, which is monotone in elevation the way the physics is.
            // k = 1 is UE exactly (SkyAtmosphereCommon.ush:271 applies the full transmittance and
            // clamps nowhere); k = 0 is a white disc. At 0.25 the sun above reads as it did before
            // -- 1.00 : 0.89 : 0.74 -- and the same sun on the horizon reaches 1.00 : 0.63 : 0.16
            // instead of staying white.
            const float kSunDiscTint = 0.25f;
            const float trPeak = max(max(max(tr.r, tr.g), tr.b), 1.e-6f);
            const float3 discTransmittance = pow(saturate(tr / trPeak), kSunDiscTint);
            float3 disk = discTransmittance * skyIlluminance.rgb / max(2.0f * SkyViewPi * oneMinusCos, 1.e-12f);
            const float diskPeak = max(max(disk.r, disk.g), disk.b);
            const float3 diskInRange = diskPeak > 65504.0f ? disk * (65504.0f / diskPeak) : disk;
            c += diskInRange * softEdge;
        }
        // The whole sky, clamped the same way: a per-channel min here would undo the disc's
        // hue again the moment any one channel touched the ceiling.
        const float cPeak = max(max(c.r, c.g), c.b);
        c = cPeak > 65504.0f ? c * (65504.0f / cPeak) : c;
    }
    float2 currUv = ClipToUV(i.clipPos);
    float2 prevUv = ClipToUV(i.prevPos);
    float2 motion = currUv - prevUv;

    PSOut o;
    o.color = float4(c, 1.0);
    o.velocity = motion;
    return o;
}
