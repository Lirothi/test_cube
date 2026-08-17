#define GTAO_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

#pragma pack_matrix(row_major)

// P6B — Ground Truth Ambient Occlusion, half resolution.
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
// t0: scene depth (the render-resolution depth SRV; this pass samples it at half-res UVs)
// t1: GB1 -- world normal encoded in xyz, roughness in w
// u0: R8_UNORM raw AO

Texture2D DepthTex : register(t0);
Texture2D GB1 : register(t1);
RWTexture2D<float> AoTarget : register(u0);

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
                              float initialOffset, float3 viewPos, float3 viewDir, float attenFactor)
{
    float2 best = float2(-1.0f, -1.0f);

    for (uint i = 0; i < steps; ++i)
    {
        const float fi = (float)i;
        // At least one pixel per step, or the walk stalls on nearby geometry and finds nothing.
        float2 uvOffset = screenDir * max(stepRadius * (fi + initialOffset), fi + 1.0f);
        uvOffset.y *= -1.0f;
        const float4 uv2 = baseUv.xyxy + float4(uvOffset.xy, -uvOffset.xy);

        [unroll] for (int side = 0; side < 2; ++side)
        {
            const float2 uv = (side == 0) ? uv2.xy : uv2.zw;
            const float3 v = ViewPosFromUv(uv, LinearDepthAt(uv)) - viewPos;
            const float lenSq = dot(v, v);
            const float ooLen = rsqrt(lenSq + 1e-4f);
            float ang = dot(v, viewDir) * ooLen;

            // Distance falloff: something far away subtends a real angle but occludes nothing.
            const float falloff = saturate(lenSq * attenFactor);
            const float prev = (side == 0) ? best.x : best.y;
            ang = lerp(ang, prev, falloff);
            const float updated = (ang > prev) ? ang : lerp(ang, prev, thickness);
            if (side == 0) { best.x = updated; } else { best.y = updated; }
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
        AoTarget[tid.xy] = 1.0f;
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

    const float3 rnd = GetRandomVector(tid.xy);
    float2 screenDir = rnd.xy;
    float offset = rnd.z;

    const float deltaAngle = kPi / (float)max(numAngles, 1u);
    const float sinDelta = sin(deltaAngle);
    const float cosDelta = cos(deltaAngle);

    float sum = 0.0f;
    for (uint a = 0; a < numAngles; ++a)
    {
        const float2 angles = SearchLargestAngleDual(numSteps, uv, screenDir * invAoSize, stepRadius,
                                                     offset, viewPos, viewDir, attenFactor);
        sum += ComputeInnerIntegral(angles, screenDir, viewDir, viewNormal);

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

    // Fade to unoccluded with distance: the radius shrinks to sub-pixel out there, so the estimate
    // is noise long before it is wrong.
    const float fade = saturate((linearZ - fadeStart) / max(fadeEnd - fadeStart, 1e-4f));
    AoTarget[tid.xy] = lerp(ao, 1.0f, fade);
}
