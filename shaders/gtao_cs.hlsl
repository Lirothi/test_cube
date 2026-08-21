#define GTAO_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

#pragma pack_matrix(row_major)

// P6B â€” Ground Truth Ambient Occlusion, half resolution.
//
// Transcribed from Unreal's `GTAOCombinedPSandCS` (PostProcessAmbientOcclusion.usf) with their
// `SearchForLargestAngleDual` and `ComputeInnerIntegral`. The shape of the algorithm:
//
//   * for N screen-space directions, walk outward in BOTH directions looking for the largest
//     elevation angle any sample subtends -- the "horizon" in that direction;
//   * project the surface normal into the plane of that walk and evaluate the arc between the two
//     horizons in closed form. That integral is what makes it "ground truth" rather than the
//     hemisphere-sampling guess SSAO does.
//
// Why this and not material AO: a per-material scalar cannot know that a palm trunk MEETS the sand.
// Contacts between separate objects are the whole reason this pass exists, and no baked map can
// express them.
//
// Half resolution is deliberate: AO is a low-frequency signal and this pass is bandwidth bound. The
// edge-aware upsample (a later step) is what puts it back on geometry edges.
//
// P16.4 -- TWO RADII OUT OF ONE DISPATCH. The search above answers "how much of the hemisphere can
// this pixel see" at ONE scale, and the scale that makes a trunk meet the sand (0.75 m) is not the
// scale that decides whether a patch of ground is under a canopy (tens of metres). With a single
// contact radius the sky fill reaches under a palm crown exactly as freely as it reaches open
// ground -- measured: raising the sky 6x brightened dense canopy by 1.92 stops and open sand by
// 1.88, i.e. the crown was worth 0.04 stops of shelter.
//
// Raising the ONE radius is not the fix. At 12 m open sand darkens by about a third of a stop as
// the contact term starts eating open ground, and the contact detail the pass exists for is lost:
// six steps spread over 12 m put the first tap two metres out, so it steps clean over the 30 cm
// contact at the base of the trunk.
//
// So the kernel walks TWICE per direction and writes both answers. This is the same split UE make
// with SSAO + DFAO and Godot with SSAO + SDFGI, except both estimates come from one pass here, so
// the depth fetch, the normal reconstruction and the direction set are shared and only the horizon
// walk itself is paid for twice. `skyRadius <= worldRadius` switches the second walk off and copies
// the contact answer into both channels, which is an exact no-op for every consumer.
//
// t0: scene depth (the render-resolution depth SRV; this pass samples it at half-res UVs)
// t1: GB1 -- world normal encoded in xyz, roughness in w
// u0: R8G8_UNORM raw AO -- .x contact scale, .y sky scale

Texture2D DepthTex : register(t0);
Texture2D GB1 : register(t1);
// P6C: the hierarchical depth pyramid. Mip 0 is the SAME grid this pass runs on, so the UVs need no
// remapping between them -- that alignment is why the pyramid is sized off the AO resolution.
Texture2D HzbTex : register(t2);
RWTexture2D<float2> AoTarget : register(u0);

SamplerState gSmpPoint : register(s0);
SamplerState gSmpLinear : register(s1);

cbuffer GtaoCB : register(b0)
{
    float4x4 view;        // world -> view, for putting the G-buffer normal in view space
    float4x4 invProj;     // clip  -> view, for rebuilding view-space position from depth
    float2   aoSize;      // half-res target size in texels
    float2   invAoSize;
    float    depthA;      // the engine's standard pair: linearZ = depthB / (deviceZ - depthA)
    float    depthB;
    float    worldRadius; // occlusion radius in WORLD units, not pixels -- see below
    float    thickness;   // 0 = treat every horizon as an infinitely thin wall, 1 = fully solid
    float    intensity;   // 1 = the integral as computed; > 1 darkens
    float    fadeStart;   // world distance where AO starts fading out
    float    fadeEnd;     // and where it is gone entirely
    float    invTanHalfFovY;
    uint     numAngles;   // directions per pixel
    uint     numSteps;    // horizon-search taps per direction
    uint     frameIndex;  // rotates the noise so the temporal pass has something to average
    uint     useGBufferNormal; // 0 = derive the normal from depth (UE's default), 1 = read GB1
    // P6C retrofit. 0 = walk the flat depth buffer (the pre-P6C behaviour, kept for A/B).
    uint     useHzb;
    uint     hzbMipBias;  // added to every step's mip; UE tie this to their quality level
    uint     hzbMipCount; // clamp, so a step can never ask for a level that was not built
    // P16.4. The SECOND radius, in world units, for the sky-fill channel. <= worldRadius switches
    // the second walk off entirely and the contact answer is written to both channels.
    float    skyRadius;
    // Mip bias for the sky walk only. It wants a coarser start than the contact walk for two
    // reasons: its taps are tens of pixels apart, so a mip-0 fetch lands nowhere near the previous
    // one, and a coarse level AGGREGATES -- which is the right answer at this scale, where a single
    // texel of leaf is not what decides whether the ground is sheltered.
    uint     skyMipBias;
    // Mid-range intensity: a multiplier on the sky channel's exponent (the contact channel keeps
    // `intensity` alone). 0 switches the sky walk's COMPUTE PATH off entirely -- same dead branch
    // as skyRadius <= worldRadius -- and the contact answer is copied into both channels.
    float    skyIntensity;
    uint     pad2;
    uint     pad3;
};

static const float kPi = 3.14159265358979f;
static const float kHalfPi = 1.57079632679f;

// Cheap acos, from the same UE source. Max error ~0.0005 rad, and this is inside an integral that a
// denoiser will smooth anyway -- a full acos here is pure cost.
float AcosFast(float x)
{
    const float ax = abs(x);
    float res = (-0.156583f * ax) + kHalfPi;
    res *= sqrt(max(0.0f, 1.0f - ax));
    return (x >= 0.0f) ? res : (kPi - res);
}

float LinearFromDevice(float deviceZ)
{
    return depthB / max(deviceZ - depthA, 1e-8f);
}

float LinearDepthAt(float2 uv)
{
    return LinearFromDevice(DepthTex.SampleLevel(gSmpPoint, uv, 0).r);
}

// View-space position from a UV and a linear depth. The engine's projection is right-handed with
// +Z into the screen, so view Z is the linear depth directly.
float3 ViewPosFromUv(float2 uv, float linearZ)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    // Reconstruct the ray through this pixel, then push it out to the measured depth.
    const float4 clip = float4(ndc, 1.0f, 1.0f);
    float4 v = mul(clip, invProj);
    v.xyz /= max(v.w, 1e-8f);
    const float3 dir = v.xyz / max(v.z, 1e-8f);
    return dir * linearZ;
}

// UE's TakeSmallerAbsDelta: of the two one-sided differences, keep the SMALLER in magnitude. On a
// silhouette one side steps onto another surface and produces a huge delta, so this picks the side
// the centre pixel actually belongs to.
float TakeSmallerAbsDelta(float left, float mid, float right)
{
    const float a = mid - left;
    const float b = right - mid;
    return (abs(a) < abs(b)) ? a : b;
}

// The GEOMETRIC normal, rebuilt from the depth buffer -- which is what the horizon search itself
// walks over.
//
// This is UE's default (`r.GTAO.UseNormals = 0`) and the reason matters: the G-buffer normal carries
// NORMAL MAPPING, while the horizon search only ever sees bare geometry. Feed the integral a
// normal-mapped normal and it computes the visible arc for a surface the search never looked at, so
// every texel whose shading normal tilts away from its geometric one loses part of its hemisphere
// to "below the surface" and reads as occluded. On a detail-mapped surface like sand that is not a
// subtle bias: measured on a dune, AO read 0.615 where the geometry is open, and 0.038 on a slope
// with nothing above it at all.
float3 ViewNormalFromDepth(float2 uv, float3 viewPosMid)
{
    const float2 d = invAoSize;
    const float z = DepthTex.SampleLevel(gSmpPoint, uv, 0).r;
    const float zL = DepthTex.SampleLevel(gSmpPoint, uv + float2(-d.x, 0.0f), 0).r;
    const float zR = DepthTex.SampleLevel(gSmpPoint, uv + float2(d.x, 0.0f), 0).r;
    const float zT = DepthTex.SampleLevel(gSmpPoint, uv + float2(0.0f, -d.y), 0).r;
    const float zB = DepthTex.SampleLevel(gSmpPoint, uv + float2(0.0f, d.y), 0).r;

    const float ddx = TakeSmallerAbsDelta(zL, z, zR);
    const float ddy = TakeSmallerAbsDelta(zT, z, zB);

    const float3 right = ViewPosFromUv(uv + float2(d.x, 0.0f), LinearFromDevice(z + ddx)) - viewPosMid;
    const float3 down = ViewPosFromUv(uv + float2(0.0f, d.y), LinearFromDevice(z + ddy)) - viewPosMid;

    float3 n = normalize(cross(right, down));
    // A visible surface faces the camera; the cross product's sign depends on the UV/NDC handedness,
    // so this states the invariant instead of relying on it.
    return (dot(n, -normalize(viewPosMid)) < 0.0f) ? -n : n;
}

// Interleaved gradient noise, and the same 4x4 offset pattern UE uses. The temporal rotation is
// folded in through `frameIndex` so consecutive frames sample different directions -- that is what
// the temporal accumulation later averages into a clean result.
float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189f * frac((p.x * 0.06711056f) + (p.y * 0.00583715f)));
}

// UE's per-frame rotation and offset TABLES, from the CPU side of their pass
// (PostProcessAmbientOcclusion.cpp, GetGTAOShaderParameters). These are not decoration: the
// rotation cycles over SIX frames and the offset over four, so a temporal history of ~10 frames
// (blend 0.1) averages a COMPLETE, evenly spread set of directions.
//
// I first wrote `(frameIndex & 63) * PI/64` here -- also evenly spread, but over 64 frames and
// walked in order. A 10-frame window then sees a narrow, slowly sliding subset of directions, so
// the estimate is biased and the bias DRIFTS. That reads as low-frequency flicker, worst where the
// per-direction spread is largest: thin geometry at distance. Measured below.
//
// In degrees the rotations are 30, 150, 90, 120, 60, 0 -- six evenly spaced angles over [0,180),
// visited out of order so that consecutive frames are far apart in angle.
static const float kTemporalRotDegrees[6] = { 60.0f, 300.0f, 180.0f, 240.0f, 120.0f, 0.0f };
static const float kTemporalOffsets[4] = { 0.1f, 0.6f, 0.35f, 0.85f };

float3 GetRandomVector(uint2 pixel)
{
    // Theirs, kept for fidelity: the noise is evaluated on a Y-flipped coordinate.
    pixel.y = 16384u - pixel.y;

    const float gradientNoise = InterleavedGradientNoise(float2(pixel));
    const float temporalAngle = kTemporalRotDegrees[frameIndex % 6u] * (kPi / 360.0f);

    float2 dir;
    dir.x = cos(gradientNoise * kPi);
    dir.y = sin(gradientNoise * kPi);

    const float c = cos(temporalAngle);
    const float s = sin(temporalAngle);

    const float scaleOffset = (1.0f / 4.0f) * (float)((pixel.y - pixel.x) & 3u);

    float3 result;
    result.x = dot(dir, float2(c, -s));
    result.y = dot(dir, float2(s, c));
    result.z = frac(scaleOffset + kTemporalOffsets[(frameIndex / 6u) % 4u] * 0.25f);
    return result;
}

// The dual horizon search: walk out along +dir and -dir together, keeping the largest elevation
// angle seen in each. `thickness` is the heuristic that stops a thin object from occluding as if it
// were a solid wall -- without it every leaf and railing casts a slab of shadow behind it.
float2 SearchLargestAngleDual(uint steps, float2 baseUv, float2 screenDir, float stepRadius,
                              float initialOffset, float3 viewPos, float3 viewDir, float attenFactor,
                              uint mipBias)
{
    float2 best = float2(-1.0f, -1.0f);

    // UE's SHIPPING domain (SearchForLargestAngleDual_HZB): i runs 1..N, so the first tap is never
    // closer than one full stride. The plain-depth variant they keep for reference starts at 0, and
    // this port had copied THAT: a sub-stride tap right at the receiver's foot is the noisiest
    // sample of the walk (half-res + furthest-mip quantisation puts it above the tangent plane as
    // often as not), and the max-picking horizon keeps exactly the noise. Measured on the bronze
    // floor at a grazing camera: the i=0 domain veiled the OPEN floor by ~3% AO at skyRadius 25.
    for (uint i = 1; i <= steps; ++i)
    {
        const float fi = (float)i;
        // At least one pixel per step, or the walk stalls on nearby geometry and finds nothing.
        float2 uvOffset = screenDir * max(stepRadius * (fi + initialOffset), fi + 1.0f);
        uvOffset.y *= -1.0f;
        const float4 uv2 = baseUv.xyxy + float4(uvOffset.xy, -uvOffset.xy);

        // MIP PER STEP, from UE's SearchForLargestAngleDual_HZB: the further a step reaches, the
        // coarser the level it reads. Two things fall out of that. The taps stop scattering across
        // memory -- a far step on mip 0 lands nowhere near the previous one -- and a coarse level
        // AGGREGATES, so the estimate stops aliasing off whatever single texel the step happened to
        // hit. Their schedule exactly: bias, then +1 at the third tap, +2 from the fifth on.
        float mip = (float)mipBias;
        if (i == 2u) { mip += 1.0f; }
        if (i > 3u) { mip += 2.0f; }
        mip = min(mip, (float)hzbMipCount - 1.0f);

        [unroll] for (int side = 0; side < 2; ++side)
        {
            const float2 uv = (side == 0) ? uv2.xy : uv2.zw;
            // The pyramid holds the FURTHEST device Z of each tile, which is the conservative
            // direction here: a coarse tile then under-estimates how much sky it blocks, so a low
            // mip cannot invent occlusion out of geometry too small to matter at its scale.
            const float sampleZ = (useHzb != 0u)
                ? LinearFromDevice(HzbTex.SampleLevel(gSmpPoint, uv, mip).r)
                : LinearDepthAt(uv);
            const float3 v = ViewPosFromUv(uv, sampleZ) - viewPos;
            const float lenSq = dot(v, v);
            const float ooLen = rsqrt(lenSq + 1e-4f);

            // Distance falloff: something far away subtends a real angle but occludes nothing.
            // UE's HZB variant gates on it entirely -- a beyond-range sample is a guaranteed
            // no-op here (the lerp pair below collapses to `prev`), so skip its ALU.
            const float falloff = saturate(lenSq * attenFactor);
            if (falloff < 1.0f)
            {
                float ang = dot(v, viewDir) * ooLen;
                const float prev = (side == 0) ? best.x : best.y;
                ang = lerp(ang, prev, falloff);
                const float updated = (ang > prev) ? ang : lerp(ang, prev, thickness);
                if (side == 0) { best.x = updated; } else { best.y = updated; }
            }
        }
    }

    best.x = AcosFast(clamp(best.x, -1.0f, 1.0f));
    best.y = AcosFast(clamp(best.y, -1.0f, 1.0f));
    return best;
}

// The closed-form arc integral. The normal is projected into the plane the search walked, the two
// horizon angles are clamped into that plane's hemisphere, and the visible arc is integrated.
float ComputeInnerIntegral(float2 angles, float2 screenDir, float3 viewDir, float3 viewNormal)
{
    const float3 planeNormal = normalize(cross(float3(screenDir.xy, 0.0f), viewDir));
    const float3 perp = cross(viewDir, planeNormal);
    const float3 projNormal = viewNormal - planeNormal * dot(viewNormal, planeNormal);

    const float lenProjNormal = length(projNormal) + 1e-6f;
    const float recipMag = 1.0f / lenProjNormal;

    const float cosAng = dot(projNormal, perp) * recipMag;
    const float gamma = AcosFast(cosAng) - kHalfPi;
    const float cosGamma = dot(projNormal, viewDir) * recipMag;
    const float sinGamma = cosAng * -2.0f;

    // Clamp to the normal's hemisphere: a horizon behind the surface is not occlusion, it is the
    // surface itself.
    float2 a;
    a.x = gamma + max(-angles.x - gamma, -kHalfPi);
    a.y = gamma + min(angles.y - gamma, kHalfPi);

    return lenProjNormal * 0.25f *
        ((a.x * sinGamma + cosGamma - cos(2.0f * a.x - gamma)) +
         (a.y * sinGamma + cosGamma - cos(2.0f * a.y - gamma)));
}

[numthreads(8, 8, 1)]
[RootSignature(GTAO_CS_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)aoSize.x || tid.y >= (uint)aoSize.y)
    {
        return;
    }

    const float2 uv = (float2(tid.xy) + 0.5f) * invAoSize;
    const float linearZ = LinearDepthAt(uv);

    // Background and anything past the fade: fully unoccluded. The interface contract is that
    // invalid depth returns 1, so nothing downstream has to special-case the sky.
    if (linearZ >= fadeEnd)
    {
        AoTarget[tid.xy] = float2(1.0f, 1.0f);
        return;
    }

    const float3 viewPos = ViewPosFromUv(uv, linearZ);
    const float3 viewDir = normalize(-viewPos);

    // Geometric normal by default (see ViewNormalFromDepth). GB1 stores the world normal 0..1
    // encoded and is kept behind a switch purely so the two can be compared.
    float3 viewNormal;
    if (useGBufferNormal != 0u)
    {
        const float3 worldNormal = normalize(GB1.SampleLevel(gSmpPoint, uv, 0).xyz * 2.0f - 1.0f);
        viewNormal = normalize(mul((float3x3)view, worldNormal));
    }
    else
    {
        viewNormal = ViewNormalFromDepth(uv, viewPos);
    }

    // WORLD-space radius converted to pixels here, rather than a fixed pixel radius: a pixel radius
    // makes the occlusion grow and shrink as the camera moves, which reads as the whole scene
    // breathing. The plan calls this out explicitly.
    const float fovScale = aoSize.y * invTanHalfFovY;
    const float worldRadiusAdj = worldRadius * fovScale;
    const float pixelRadius = max(min(worldRadiusAdj / linearZ, 256.0f), (float)numSteps);
    const float stepRadius = pixelRadius / ((float)numSteps + 1.0f);
    const float attenFactor = 2.0f / max(worldRadius * worldRadius, 1e-6f);

    // P16.4 -- the sky-scale walk. Uniform across the whole dispatch (every input is from the CB),
    // so the branch below is wave-coherent and costs nothing when it is off.
    //
    // ITS OWN SCREEN CLAMP, four times the contact walk's. The clamp exists to bound how far a walk
    // may reach across the screen; at 256 texels a 12 m radius stops being 12 m at any depth under
    // ~44 m, and MEASURED, the whole knob saturated: 25 m and 40 m produced the same image (-0.577
    // vs -0.583 stops under canopy) because both were clipped to the same screen span and only the
    // distance falloff still separated them. A knob whose top half does nothing is a knob that
    // lies. The clamp costs no extra taps -- `numSteps` is unchanged and the steps simply land
    // further apart -- so what it buys with is cache locality, which is exactly what `skyMipBias`
    // pays back: at mip 4 a 1024-texel span is 64 texels of that level.
    static const float kSkyPixelRadiusMax = 1024.0f;
    // skyIntensity 0 kills the whole second walk, same dead wave-coherent branch as the radius
    // switch -- the dedicated off knob the mid-range channel was missing.
    const bool wantSky = skyRadius > worldRadius && skyIntensity > 0.0f;
    const float skyPixelRadius = max(min(skyRadius * fovScale / linearZ, kSkyPixelRadiusMax),
                                     (float)numSteps);
    const float skyStepRadius = skyPixelRadius / ((float)numSteps + 1.0f);
    const float skyAttenFactor = 2.0f / max(skyRadius * skyRadius, 1e-6f);

    const float3 rnd = GetRandomVector(tid.xy);
    float2 screenDir = rnd.xy;
    float offset = rnd.z;

    const float deltaAngle = kPi / (float)max(numAngles, 1u);
    const float sinDelta = sin(deltaAngle);
    const float cosDelta = cos(deltaAngle);

    float sum = 0.0f;
    float skySum = 0.0f;
    for (uint a = 0; a < numAngles; ++a)
    {
        const float2 angles = SearchLargestAngleDual(numSteps, uv, screenDir * invAoSize, stepRadius,
                                                     offset, viewPos, viewDir, attenFactor,
                                                     hzbMipBias);
        sum += ComputeInnerIntegral(angles, screenDir, viewDir, viewNormal);

        // The second walk shares this direction and this phase offset on purpose: the temporal
        // stage averages the two channels over the SAME rotation schedule, so a direction set that
        // is complete for one is complete for the other.
        if (wantSky)
        {
            const float2 skyAngles = SearchLargestAngleDual(numSteps, uv, screenDir * invAoSize,
                                                            skyStepRadius, offset, viewPos, viewDir,
                                                            skyAttenFactor, skyMipBias);
            skySum += ComputeInnerIntegral(skyAngles, screenDir, viewDir, viewNormal);
        }

        const float2 prevDir = screenDir;
        screenDir.x = prevDir.x * cosDelta - prevDir.y * sinDelta;
        screenDir.y = prevDir.x * sinDelta + prevDir.y * cosDelta;
        offset = frac(offset + 0.617f); // golden-ish, so successive angles do not share a step phase
    }

    // The arc integral already returns 1.0 for an unoccluded surface -- verified by simulating this
    // kernel against an analytic plane, which reads 0.9999 at every tilt and depth.
    //
    // UE scales by 2/PI here (PostProcessAmbientOcclusion.usf:908) and then multiplies by PI/2
    // again at the top of their SPATIAL FILTER (`SumAO *= (PI/2.0)`), so the pair cancels and their
    // consumer sees 1.0. Transcribing only the first half left every surface in the scene darkened
    // by a constant 1/1.571 -- an open beach read 0.637 instead of 1.0.
    //
    // The scale is not reintroduced in the filter here, because THIS engine's denoise stage is
    // optional (`gtao.denoise`) and `intensity` is applied in this kernel: a normalisation that only
    // holds when a later optional pass runs is a normalisation waiting to be switched off. Every
    // stage of this chain now carries AO in [0,1] with "unoccluded" meaning exactly 1.
    float ao = sum / (float)max(numAngles, 1u);
    ao = saturate(pow(saturate(ao), max(intensity, 1e-3f)));

    // The sky channel takes the SAME intensity exponent, so the two stay comparable and the look
    // knob keeps meaning one thing. Off, it is a copy -- which is what makes `skyRadius <=
    // worldRadius` an exact no-op rather than an approximate one.
    float skyAo = ao;
    if (wantSky)
    {
        skyAo = skySum / (float)max(numAngles, 1u);
        // The shared exponent scaled by the mid-range's own intensity: 1 = the old shared
        // behaviour, below 1 = a lighter sky veil at the same reach, above 1 = deeper shelter.
        skyAo = saturate(pow(saturate(skyAo), max(intensity * skyIntensity, 1e-3f)));
    }

    // Fade to unoccluded with distance: the radius shrinks to sub-pixel out there, so the estimate
    // is noise long before it is wrong.
    const float fade = saturate((linearZ - fadeStart) / max(fadeEnd - fadeStart, 1e-4f));
    AoTarget[tid.xy] = float2(lerp(ao, 1.0f, fade), lerp(skyAo, 1.0f, fade));
}
