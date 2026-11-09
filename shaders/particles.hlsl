// Part E2: GPU particle billboards. The VS reads the sim's particle buffer (t0) directly —
// no vertex buffer and no per-frame CPU upload; 6 vertices per slot via SV_VertexID. Dead
// slots (age < 0) emit zero-w degenerate triangles, so the draw covers maxParticles slots
// unconditionally and needs no alive-list or indirect args. Draws inside Pass_Transparent
// (depth-test GREATER_EQUAL reversed-Z, no depth write; RT0 blend only — velocity/bias/id
// targets are write-masked off in the PSO).
#pragma pack_matrix(row_major)

struct Particle
{
    float3 pos; float age;
    float3 vel; float life;
    float rot; float spin; uint seed; float _pad;
};

// Mirror of the Pass_Transparent per-view CB (BuildGlassViewCB / GlassView in glass.hlsl) —
// declared up to the last field this shader consumes; trailing fields are irrelevant to a
// root-CBV bind.
// b1 = the transparent pass's shared per-view CB (GlassView layout); the full layout comes from
// the include so the fog fields at its tail are reachable (plan A5). Needs the VSM level count.
#include "vsm_addressing.hlsli"
#include "glass_view_cb.hlsli"
#include "fog_common.hlsli"

// Mirror of vfx::GpuEmitterDrawParams (ParticleTypes.h).
cbuffer DrawParams : register(b2)
{
    float sizeStart; float sizeEnd; uint flipCols; uint flipRows;
    float flipFps; uint flipRandomStart; uint frameBlend; uint hasTexture;
    float4 colorKeys[4];
    // depthOcclude: 1 = occlude/soft-fade against the OPAQUE depth copy in the PS. Transparent
    // surfaces are handled by the hardware depth test after they render. softFadeDist is the
    // soft-fade width (0 = hard cutoff at the opaque surface).
    // preExposure: P16.1. This pass writes into scene colour AFTER compose, so compose's
    // scaling never reaches it and it applies the same factor itself. 1.0 = not pre-exposed.
    uint maxParticles; float softFadeDist; float depthOcclude; float preExposure;
    // P16.7: what the authored colour MEANS, in cd/m2. See ParticleTypes.h.
    // lit: the colour is an ALBEDO lit by the frame's light and the air (vfx::EmitterDesc::lit); forwardScatter of the
    // sun's part goes through Henyey-Greenstein(phaseG), the rest a wrapped Lambert on the sphere the sprite stands for.
    float luminanceCdM2; float lit; float forwardScatter; float phaseG;
    // velocityStretch: the quad is size + |v across the view| x this (s) long, along that velocity, no roll.
    // skyIrradianceScale: lit, the scale of the sky's irradiance cube (t5); 0 = none (the fog's colour).
    // litFlat: lit, the shading normal from the sprite's sphere (0) to the world's up (1: lit as the water's foam).
    float velocityStretch; float skyIrradianceScale; float litFlat; float _dpPad;
};

StructuredBuffer<Particle> gParticles : register(t0);
Texture2D gSprite : register(t1);
Texture2D gSceneDepth : register(t2); // E2b: depthCopy (reversed-Z NDC), soft-particle fade
StructuredBuffer<uint> gSorted : register(t3); // E2c: back-to-front slot order (alpha emitters)
Texture3D<float4> gFogVolume : register(t4);   // plan A5: the integrated froxel volume (gated by fogVolumeParams.x)
TextureCube gSkyIrradiance : register(t5);     // lit: the sky's irradiance (E / pi), lighting_cs's (gated by skyIrradianceScale)
SamplerState gSmp : register(s0);

#define PARTICLES_RS \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(SRV(t0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

#ifndef PARTICLE_SORTED
#define PARTICLE_SORTED 0
#endif

// View-space Z from a (reversed-Z) NDC depth using the projection coefficients (independent of
// screen xy for a standard perspective proj). Far plane (NDC 0) -> huge Z, so sky never fades.
float ViewZFromNdc(float ndcZ)
{
    return proj._m32 / (ndcZ * proj._m23 - proj._m22);
}

struct VSOut
{
    float4 H : SV_POSITION;
    float4 color : COLOR0;
    float2 uvA : TEXCOORD0;   // flipbook frame A sub-uv
    float2 uvB : TEXCOORD1;   // frame B sub-uv (frameBlend)
    float2 disc : TEXCOORD2;  // quad-local [-1..1] for the procedural sprite fallback
    float blendF : TEXCOORD3;
    float3 axisX : TEXCOORD4; // lit: the quad's world axes, for the normal of the sphere the sprite stands for
    float3 axisY : TEXCOORD5;
    float3 toEye : TEXCOORD6; // lit: from the particle to the camera, unit
};

float4 ColorOverLife(float t)
{
    // 4-key gradient over normalized age (keys at t = 0, 1/3, 2/3, 1).
    float s = saturate(t) * 3.0;
    uint k = min(uint(s), 2u);
    float f = saturate(s - float(k));
    return lerp(colorKeys[k], colorKeys[k + 1u], f);
}

float2 FrameUv(uint frame, float2 quadUv)
{
    uint frames = max(flipCols * flipRows, 1u);
    frame = frame % frames;
    float2 cell = float2(frame % flipCols, frame / flipCols);
    return (cell + quadUv) / float2(flipCols, flipRows);
}

[RootSignature(PARTICLES_RS)]
VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o = (VSOut)0;

    const uint ordinal = vid / 6u;
    const uint corner = vid % 6u;
    if (ordinal >= maxParticles) { return o; } // H = 0 -> clipped

    // E2c: alpha emitters draw in the sorted (far-to-near) order; additive ones index directly.
    const uint slot = PARTICLE_SORTED ? gSorted[ordinal] : ordinal;

    Particle p = gParticles[slot];
    if (p.age < 0.0 || p.life <= 0.0) { return o; } // dead slot -> degenerate quad

    const float t = saturate(p.age / p.life);

    // Camera-facing basis from the inverse view (row-vector math: world basis = invView rows).
    const float3 rightWS = normalize(invView[0].xyz);
    const float3 upWS = normalize(invView[1].xyz);

    // Two CCW triangles: (-1,-1)(1,-1)(-1,1) and (1,-1)(1,1)(-1,1).
    const float2 corners[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(1, -1),  float2(1, 1),  float2(-1, 1)
    };
    float2 c = corners[corner];

    const float size = lerp(sizeStart, sizeEnd, t) * 0.5;
    const float3 toEye = normalize(camPosSky.xyz - p.pos);
    float3 axisX;
    float3 axisY;
    float2 lc;
    // The velocity seen across the view: a stretched quad lies along it, as long as the particle flies in
    // velocityStretch seconds plus its size; a particle flying at the eye stays round.
    const float3 across = p.vel - toEye * dot(p.vel, toEye);
    const float acrossLen = length(across);
    if (velocityStretch > 0.0 && acrossLen > 1.0e-3)
    {
        axisY = across / acrossLen;
        axisX = normalize(cross(axisY, toEye));
        lc = float2(c.x * size, c.y * (size + 0.5 * acrossLen * velocityStretch));
    }
    else
    {
        const float cs = cos(p.rot);
        const float sn = sin(p.rot);
        axisX = rightWS * cs + upWS * sn;
        axisY = upWS * cs - rightWS * sn;
        lc = c * size;
    }

    const float3 wpos = p.pos + axisX * lc.x + axisY * lc.y;
    o.H = mul(float4(wpos, 1.0), viewProj);
    o.axisX = axisX;
    o.axisY = axisY;
    o.toEye = toEye;

    o.color = ColorOverLife(t);
    o.disc = c;

    const float2 quadUv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5); // v flipped (sprite space)
    const uint frames = max(flipCols * flipRows, 1u);
    uint frameA = 0u;
    float f = 0.0;
    if (flipFps > 0.0)
    {
        float fpos = p.age * flipFps + (flipRandomStart != 0u ? float(p.seed % frames) : 0.0);
        frameA = uint(fpos);
        f = frac(fpos);
    }
    else if (flipRandomStart != 0u)
    {
        frameA = p.seed % frames;
    }
    o.uvA = FrameUv(frameA, quadUv);
    o.uvB = FrameUv(frameA + 1u, quadUv);
    o.blendF = frameBlend != 0u ? f : 0.0;
    return o;
}

[RootSignature(PARTICLES_RS)]
float4 PSMain(VSOut i) : SV_Target0
{
    float4 tex;
    if (hasTexture != 0u)
    {
        tex = gSprite.Sample(gSmp, i.uvA);
        if (i.blendF > 0.0)
        {
            tex = lerp(tex, gSprite.Sample(gSmp, i.uvB), i.blendF);
        }
    }
    else
    {
        // Procedural soft disc (asset-free fallback / sparks).
        float r = saturate(1.0 - dot(i.disc, i.disc));
        tex = float4(1.0, 1.0, 1.0, r * r);
    }

    float4 c = i.color * tex;

    // Depth occlusion + E2b soft particles, done in the PS against the OPAQUE depth copy (the
    // depth snapshotted before this pass — i.e. BEFORE the ocean/glass draw, so only real opaque
    // geometry drives the soft intersection fade rather than the transparent water surface).
    // The hardware depth test still handles the current scene depth, including water rendered
    // before particles. dz > 0 => particle is IN FRONT of the opaque surface (visible; soft-faded
    // near it); dz <= 0 => behind opaque geometry (fully occluded).
    if (depthOcclude > 0.0)
    {
        float sceneNdc = gSceneDepth.Load(int3(int2(i.H.xy), 0)).r;
        float dz = ViewZFromNdc(sceneNdc) - ViewZFromNdc(i.H.z);
        float fade = softFadeDist > 0.0 ? saturate(dz / softFadeDist) : (dz > 0.0 ? 1.0 : 0.0);
        c.a *= fade;
    }

    // P16.1: the factor goes on the COLOUR only -- alpha is a blend weight against a
    // destination that already carries it, and scaling it would change the coverage, not the
    // brightness.
    // P16.7: the authored colour is a HUE; `luminanceCdM2` is what it is worth in the units
    // scene colour is actually in. Without it an authored 1.0 is a thousandth of a lit scene
    // and an alpha-blended particle SUBTRACTS from the frame instead of adding to it.
    // Volumetric fog (plan A5): the volume's transmittance dims the particle and its in-scatter is
    // added in proportion to the particle's coverage (the output is premultiplied). Only the volume:
    // the analytic medium beyond its far plane is not applied here (particles live near the camera,
    // and the sky lookup it needs is not bound to this pass).
    float3 fogged = c.rgb * luminanceCdM2;
    if (lit > 0.5)
    {
        // The frame's directional light (the sun or the moon, lux) on a cloud of drops: the water foam's wrapped Lambert
        // (ocean LitFoamColor: 0.2 + 0.8 N.L) and a forward lobe (Henyey-Greenstein): water drops send the light on, so
        // spray against the sun glows. The air's light is the sky's fill at the normal, as the foam takes it (x 1 + 0.3
        // (1 - N.y)) -- the irradiance cube lighting_cs fills an opaque surface from (the sky by day; the moon's sky, the
        // night sky and Ultra Dynamic Sky's layer by night) --, else the height fog's colour (UE's fog inscatter), else
        // the level's ambient share of the sun. The normal goes from the sphere the sprite stands for to the world's up
        // by litFlat: at 1 the spray is lit as a level sheet of foam, the white water it falls into (2026-11-09, owner:
        // "the trail is white white and the particles react to the light much more" -- as spheres a low warm sun took
        // their sides and the sea under them took their sky: at 18:00 B/R 0.78 against the trail's 0.87, 23 levels
        // darker). The fog's colour came first until that day (owner: "the spray is bluish"): it carries the
        // atmosphere's HeightFogContribution (UDS's 2 by day) and the fog's own floor. The atmosphere's distant sky
        // light alone was tried: it has no night in it (black spray).
        const float3 lightTravel = normalize(sunDirAmbient.xyz);
        const float3 sunE = sunColorExposure.xyz * sunColorExposure.w;
        const float r2 = saturate(dot(i.disc, i.disc));
        const float3 sphereN = normalize(i.axisX * i.disc.x + i.axisY * i.disc.y + i.toEye * sqrt(1.0 - r2));
        const float3 N = normalize(lerp(sphereN, float3(0.0, 1.0, 0.0), saturate(litFlat)) + float3(0.0, 1.0e-4, 0.0));
        const float wrap = 0.2 + 0.8 * saturate(dot(N, -lightTravel));
        const float g = phaseG;
        const float denom = max(1.0 + g * g - 2.0 * g * dot(lightTravel, i.toEye), 1.0e-4);
        const float hg = (1.0 - g * g) / (4.0 * 3.14159265 * denom * sqrt(denom));
        // As a sphere the forward lobe takes its share from the diffuse (one drop's phase function); as the foam the diffuse
        // is the foam's whole and the lobe the light a sheet of drops passes on, which the foam on the water cannot.
        const float diffuseShare = lerp(1.0 - forwardScatter, 1.0, saturate(litFlat));
        const float3 sun = sunE * (diffuseShare * wrap * (1.0 / 3.14159265) + forwardScatter * hg);
        const float3 air = skyIrradianceScale > 0.0
            ? gSkyIrradiance.SampleLevel(gSmp, N, 0.0).rgb * skyIrradianceScale * (1.0 + 0.3 * (1.0 - N.y))
            : (fogInscatter.w > 0.5 ? fogInscatter.rgb : sunE * sunDirAmbient.w);
        fogged = c.rgb * (sun + air);
    }
    {
        const float viewDepth = max(ViewZFromNdc(i.H.z), 1.0e-4f);
        const float2 fogUv = i.H.xy * screenSizeInv.zw;
        const float4 vol = FogVolumeSampleAt(gFogVolume, gSmp, fogUv, viewDepth, fogVolumeParams, fogVolumeZParams);
        fogged = fogged * vol.a + vol.rgb;
    }
    return float4(fogged * c.a * preExposure, c.a); // premultiplied
}
