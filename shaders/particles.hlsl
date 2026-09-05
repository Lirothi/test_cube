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
    float luminanceCdM2; float3 _dpPad;
};

StructuredBuffer<Particle> gParticles : register(t0);
Texture2D gSprite : register(t1);
Texture2D gSceneDepth : register(t2); // E2b: depthCopy (reversed-Z NDC), soft-particle fade
StructuredBuffer<uint> gSorted : register(t3); // E2c: back-to-front slot order (alpha emitters)
Texture3D<float4> gFogVolume : register(t4);   // plan A5: the integrated froxel volume (gated by fogVolumeParams.x)
SamplerState gSmp : register(s0);

#define PARTICLES_RS \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
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
    const float cs = cos(p.rot);
    const float sn = sin(p.rot);
    const float2 lc = float2(c.x * cs - c.y * sn, c.x * sn + c.y * cs) * size;

    const float3 wpos = p.pos + rightWS * lc.x + upWS * lc.y;
    o.H = mul(float4(wpos, 1.0), viewProj);

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
    {
        const float viewDepth = max(ViewZFromNdc(i.H.z), 1.0e-4f);
        const float2 fogUv = i.H.xy * screenSizeInv.zw;
        const float4 vol = FogVolumeSampleAt(gFogVolume, gSmp, fogUv, viewDepth, fogVolumeParams, fogVolumeZParams);
        fogged = fogged * vol.a + vol.rgb;
    }
    return float4(fogged * c.a * preExposure, c.a); // premultiplied
}
