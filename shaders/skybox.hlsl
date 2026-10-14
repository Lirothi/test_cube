#define SKYBOX_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "sky_view_mapping.hlsli"
#include "night_sky.hlsli"

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
    // Time-of-day plan N1: the moon (SkyViewFrameData light 1 and its disc).
    float4 skyLight1Direction;   // local XYZ to the moon; w its disc's angular radius (0 = no disc)
    float4 skyLight1Illuminance; // outer-space RGB lux as lit now; w the earthshine fraction of the full disc
    float4 skyMoonDisc;          // the FULL moon's outer-space RGB lux; w the phase angle (rad)
    float4 skyMoonDisc2;         // x the disc's lit scale; y 1 = the albedo/normal texture is ready; z the eye's adaptation to the disc (0..1); w the bright limb's position angle (rad, 0 = r, pi/2 = u)
    float4 skyMoonGlow;          // N4: x the glow's peak, a fraction of the FULL moon's mean disc radiance (phase law applied); y its radius (rad); zw 0
    // Time-of-day plan N2: the night sky (NightSkyFrameData).
    float4 nightGlow;            // rgb the airglow's zenith radiance (cd/m2); w the stars' + Milky Way's mean
    float4 nightLayers;          // x stars, y Milky Way (0 = its map not ready), z twinkle 0..1, w time (s)
    float4 nightRow0;            // world -> equatorial by rows; w 1 = the night sky is drawn
    float4 nightRow1;
    float4 nightRow2;
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
Texture2D<float4> moonTexture : register(t3); // N1: normal (xyz, the disc's frame) + relative albedo (w)
Texture2D<float4> nightMap : register(t4);    // N2: the Milky Way, equatorial equirectangular (night_sky.hlsli)
SamplerState samLinear : register(s0);

// THE EYE'S LOCAL ADAPTATION TO THE MOON (N4, skyMoonDisc2.z, 0..1): the scale that brings the FULL moon's
// mean disc radiance to kMoonDiscDisplay of the exposed scale (middle grey 0.18) -- the disc's and its glow's.
// Blended in log2 by the knob; 1 when the moon is under it already or the knob is 0. See the disc below.
static const float kMoonDiscDisplay = 1.5f;
float MoonDiscAdaptation(float3 fullMeanRadiance)
{
    const float fullMean = dot(fullMeanRadiance, float3(0.2126f, 0.7152f, 0.0722f)) * skyExposure.x;
    const float adapt = saturate(skyMoonDisc2.z);
    return (adapt > 0.0f && fullMean > kMoonDiscDisplay) ? exp2(adapt * log2(kMoonDiscDisplay / fullMean)) : 1.0f;
}

// The atmosphere's transmittance from the eye toward `dir` (the LUT's local frame), as a disc takes it:
// UE SkyAtmosphereCommon.ush:256-279 with the Bruneton mapping inline. The sun's and the moon's.
float3 DiscTransmittance(float3 dir, float height, float bottom, float top)
{
    const float H = sqrt(top * top - bottom * bottom);
    const float h = min(height, top);
    const float rho = sqrt(max(0.0f, h * h - bottom * bottom));
    const float d = max(0.0f, -h * dir.z + sqrt(max(0.0f, h*h*(dir.z*dir.z-1.0f)+top*top)));
    const float2 uv = float2((d - (top-h)) / max(1.e-6f, rho+H-(top-h)), rho/H);
    return height >= top ? 1.0f : transmittanceLut.SampleLevel(samLinear, uv, 0).rgb;
}

struct PSOut
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
};

[RootSignature(SKYBOX_RS)]
PSOut PSMain(VSOut i)
{
    float3 c = sky.Sample(samLinear, i.dir).rgb * exposure;
    // N2: one pixel's angular size, for the stars' point footprint -- taken here, in uniform flow.
    const float3 worldDir = normalize(i.dir);
    const float pixelAngle = max(length(ddx(worldDir)), length(ddy(worldDir)));
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
            float3 tr = DiscTransmittance(dir, height, bottom, top);
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
        // TIME-OF-DAY PLAN N2: THE NIGHT SKY'S OWN LIGHT (night_sky.hlsli) -- the airglow, the Milky Way and
        // the stars, each through the air in front of it (the transmittance to the top of the atmosphere,
        // the discs' own): they redden and fade toward the horizon. The airglow above the horizon is held
        // at its horizon value below it, as the rest of the drawn sky is; the galaxy and the stars stop
        // there. Added after the moon, which hides the stars and the galaxy behind it -- but not the
        // airglow: that layer is 90 km up, IN FRONT of the moon, and hidden by it the dark side of a
        // crescent showed as a disc darker than the sky (owner, 2026-10-13).
        float3 night = 0.0f, airglow = 0.0f;
        [branch] if (nightRow0.w > 0.0f)
        {
            const float up = max(dir.z, 0.0f);
            airglow = nightGlow.rgb * NightAirglowVanRhijn(up);
            if (dir.z > 0.0f)
            {
                const float3 eq = float3(dot(nightRow0.xyz, worldDir), dot(nightRow1.xyz, worldDir), dot(nightRow2.xyz, worldDir));
                const float3 galaxy = nightMap.SampleLevel(samLinear, NightMapUv(eq), 0).rgb * nightLayers.y;
                night += galaxy * kNightMapUnit;
                // The faint stars crowd toward the galaxy's plane: where its band is bright, all of them.
                const float density = saturate(0.3f + 0.35f * dot(galaxy, float3(0.2126f, 0.7152f, 0.0722f)));
                const float horizonWeight = 0.25f + 0.75f * (1.0f - dir.z) * (1.0f - dir.z);
                night += NightStars(eq, pixelAngle, density, nightLayers.w, nightLayers.z, horizonWeight) * nightLayers.x;
            }
            const float3 nightTr = DiscTransmittance(normalize(float3(dir.xy, up)), height, bottom, top);
            night *= nightTr;
            airglow *= nightTr;
        }
        float moonCover = 0.0f;
        // TIME-OF-DAY PLAN N1: THE MOON'S DISC. UE draw a disc per atmosphere light
        // (SkyAtmosphere.usf:357 GetLightDiskLuminance(.., LightIndex)); theirs is a flat disc, ours
        // has a face and a phase:
        //   * the surface is the procedural near side (sky_moon_cs.hlsl): its normal and its albedo;
        //   * it is shaded by LOMMEL-SEELIGER, mu0 / (mu0 + mu), not Lambert -- the real full moon is as
        //     bright at its limb as at its centre, and Lambert would darken the limb into a ball;
        //   * the sun lights it from the PHASE ANGLE TimeOfDay computed (the simple path's phase knob,
        //     or the real geometry), turned toward where the sun actually is, so the bright limb faces it;
        //   * the disc integrates to the full moon's illuminance times the phase law the moon's LIGHT
        //     follows (skyMoonDisc2.x = phase law / the Lommel-Seeliger integral) -- the disc and its
        //     light cannot disagree about how bright the moon is;
        //   * the dark side carries the earthshine;
        //   * the air in front of it attenuates it by the full transmittance, as physics has it (the
        //     moon sits far below the FP16 ceiling, so the sun's hue constant has nothing to do here).
        // Opaque: it hides what is behind it (the stars, when N2 adds them); the in-scattering in
        // front of it, already in `c`, stays.
        {
            const float moonRadius = skyLight1Direction.w;
            const float3 f = skyLight1Direction.xyz; // eye -> moon
            const float3 upRef = abs(f.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
            const float3 u = normalize(upRef - dot(upRef, f) * f);
            const float3 r = cross(f, u);
            const float invSin = 1.0f / max(sin(moonRadius), 1.e-6f);
            const float2 disc = float2(dot(dir, r), dot(dir, u)) * invSin; // the unit disc, x right, y up
            const float2 moonUv = disc * float2(0.5f, -0.5f) + 0.5f;
            // Gradients taken HERE, in uniform flow, for the textured sample inside the branch.
            const float2 uvDx = ddx(moonUv), uvDy = ddy(moonUv);
            const float rr = dot(disc, disc);
            const float edgeWidth = max(fwidth(sqrt(rr)), 1.e-4f);
            // THE MOON'S GLOW (N4; owner, 2026-10-13, with Ultra Dynamic Sky's moon shot with its shafts OFF:
            // "the halo is different, and look at the length"). UDS draws round its moon an additive glow,
            // (1 - angle / Moon Glow Scale)^2 x Moon Glow Intensity of the moon's emissive (its Moon material
            // function; live 0.05 of the emissive -- ~16 % of the mean disc -- out to 11.6 degrees), and its light
            // shafts seed on THAT: they only widen it by ~40 %. Ours stood on a bare sky, the disc alone seeded
            // the shafts and the whole halo was theirs -- concentrated, three times UDS's contrast. The real
            // moon's is the aureole of the haze and ice in front of it (the sky's Mie term with UE's clean-air
            // aerosol gives ~1.3x the sky there). skyMoonGlow.x of the full moon's mean disc radiance (the phase
            // law applied on the CPU), in the disc's colour and in the scale the eye sees the disc in, through
            // the air like the disc.
            if (!ground && moonRadius > 0.0f && skyMoonGlow.x > 0.0f && skyMoonGlow.y > 0.0f)
            {
                const float glowT = saturate(1.0f - acos(clamp(dot(dir, f), -1.0f, 1.0f)) / skyMoonGlow.y);
                if (glowT > 0.0f)
                {
                    const float glowSolidAngle = 4.0f * SkyViewPi * pow(sin(0.5f * moonRadius), 2.0f);
                    const float3 fullMeanRadiance = skyMoonDisc.rgb / max(glowSolidAngle, 1.e-12f);
                    c += fullMeanRadiance * (skyMoonGlow.x * glowT * glowT) * MoonDiscAdaptation(fullMeanRadiance) *
                         DiscTransmittance(dir, height, bottom, top);
                }
            }
            if (!ground && moonRadius > 0.0f && dot(dir, f) > 0.0f && rr < 1.0f + 2.0f * edgeWidth)
            {
                float3 nDisc = float3(disc, sqrt(max(0.0f, 1.0f - rr)));
                float albedo = 1.0f;
                if (skyMoonDisc2.y > 0.0f)
                {
                    // (A telephoto shot of this disc at EV 14 comes out of DLSS posterized to whole
                    // stops. Not the texture -- a smooth sphere does it too -- but NGX's own auto-
                    // exposure, which DlssHandler keeps on for the water, metering a frame that is
                    // black but for the disc. A real night frame is not that; test the disc with
                    // --dlss=off.)
                    const float4 t = moonTexture.SampleGrad(samLinear, moonUv, uvDx, uvDy);
                    nDisc = normalize(t.xyz);
                    albedo = t.w;
                }
                // A facet the viewer can see faces the viewer: a bent normal pointing away is the bump
                // map overreaching, and with the sun behind the eye (full moon) it drew black dots on a
                // disc that has no shadow at all. Keep every normal at least slightly toward the eye.
                nDisc.z = max(nDisc.z, 0.2f);
                nDisc = normalize(nDisc);
                const float3 v = -f; // moon -> eye
                const float3 n = nDisc.x * r + nDisc.y * u + nDisc.z * v;
                // The bright limb's direction across the disc, by its position angle (TimeOfDay: toward a
                // virtual sun on the moon's own circle in Simple mode -- the moon no longer moves with the
                // phase, and the real sun behind the viewer could not tell waxing from waning -- the real
                // sun in Real mode).
                const float3 towardSun = cos(skyMoonDisc2.w) * r + sin(skyMoonDisc2.w) * u;
                const float alpha = skyMoonDisc.w;
                const float3 s = cos(alpha) * v + sin(alpha) * towardSun; // moon -> sun, at the phase angle
                // The relief bends the INCIDENCE only; the emergence is the sphere's own, so a crater
                // wall near the limb cannot turn the surface away from the viewer into a black dot.
                const float mu0 = dot(n, s), mu = max(sqrt(max(0.0f, 1.0f - rr)), 1.e-3f);
                // THE TERMINATOR OVER A PIXEL AND A HALF (N4): Lommel-Seeliger rises from zero at its steepest,
                // so the lit edge stepped in one pixel and stair-stepped along a quarter moon's straight
                // terminator (owner, 2026-10-13). mu0 goes through a smooth maximum with 0 over the disc's own
                // pixel width (edgeWidth: the unit disc per pixel, as the limb's coverage uses), so the
                // terminator blends across ~1.5 pixels at any size the disc is drawn -- and the result is
                // cut by a smooth step over the same width, so past the terminator it is exactly dark (alone,
                // the smooth maximum never reaches 0, and where mu falls to 0 at the limb it lit a bright
                // ring round the whole dark side).
                const float terminatorWidth = 1.5f * edgeWidth;
                const float mu0Soft = 0.5f * (mu0 + sqrt(mu0 * mu0 + terminatorWidth * terminatorWidth));
                const float lommelSeeliger = mu0Soft / (mu0Soft + mu) * smoothstep(-terminatorWidth, terminatorWidth, mu0);
                // 2 pi (1 - cos r), cancellation-free. A full disc at LS 0.5 everywhere integrates to
                // skyMoonDisc.rgb, hence the 2 / solid angle.
                const float solidAngle = 4.0f * SkyViewPi * pow(sin(0.5f * moonRadius), 2.0f);
                const float3 perUnit = skyMoonDisc.rgb * (2.0f / max(solidAngle, 1.e-12f)) * albedo;
                float3 moon = perUnit * (lommelSeeliger * skyMoonDisc2.x + 0.5f * skyLight1Illuminance.w);
                // THE EYE'S LOCAL ADAPTATION TO THE DISC (skyMoonDisc2.z, 0..1). Under a night exposure the
                // lit moon is ~10^4 times the display's white: the whole of it clipped flat, the maria
                // gone, and every pixel the limb or the terminator only partly covers still white -- the
                // edge cannot antialias and stair-steps (owner, 2026-10-13: "the terminator is very
                // pixelated, the moon's pattern is not visible"). The eye adapts to the bright disc on its
                // own and sees its face; so the disc is scaled, uniformly, until the FULL moon's mean
                // radiance sits at kMoonDiscDisplay of the exposed scale (middle grey 0.18) -- the maria
                // and the relief read, the edges blend. Blended in log2 by the knob (0 = the physical,
                // clipped disc a camera records). A crescent keeps its surface brightness against the
                // full moon, and a low moon its reddening and dimming by the air.
                moon *= MoonDiscAdaptation(skyMoonDisc.rgb / max(solidAngle, 1.e-12f));
                const float coverage = saturate((1.0f - sqrt(rr)) / edgeWidth + 0.5f);
                c += moon * DiscTransmittance(dir, height, bottom, top) * coverage;
                moonCover = coverage;
            }
        }
        c += airglow + night * (1.0f - moonCover);
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
