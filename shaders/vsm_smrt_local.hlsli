// SMRT for a LOCAL light: UE's TraceLocalLight (VirtualShadowMapProjectionSpot.ush:289-489, with SMRTSingleRayInitialize, SMRTFindSample and the
// two-face ray above it there). One to a pixel and light, in lights' own passes. Metres where theirs are centimetres, direct Z where theirs is
// reversed: the flips are marked. r.Shadow.Virtual.SMRT.RayCountLocal 4 and SamplesPerRayLocal 4 on High. A light of no size (SourceRadius 0, UE's
// default) takes no steps along its rays: each is one sample at the receiver, moved by the dither.
//
// What is not theirs: a sample reads from the mip the receiver's page is on and walks to coarser ones (UE reads from mip 0 by a table flattened per
// frame, PropagateMappedMips: 16384 threads a view, 2 M at our 128 views, for the one lookup that walk does in a few reads).
#ifndef VSM_SMRT_LOCAL_HLSLI
#define VSM_SMRT_LOCAL_HLSLI
#include "vsm_sample.hlsli"

struct VsmLocalSmrtParams
{
    uint  rayCount;            // r.Shadow.Virtual.SMRT.RayCountLocal (High 4)
    uint  samplesPerRay;       // SamplesPerRayLocal (High 4)
    uint  adaptiveRayCount;    // AdaptiveRayCount (1)
    uint  frameIndex;          // View.StateFrameIndex, 0 = a still pattern
    float texelDitherScale;    // TexelDitherScaleLocal (2)
    float extrapolateMaxSlope; // ExtrapolateMaxSlopeLocal (0.05)
    float maxSlopeBias;        // MaxSlopeBiasLocal (50)
    float cotMaxRayAngle;      // 1 / tan(MaxRayAngleFromLight 0.03)
    float normalBias;          // r.Shadow.Virtual.NormalBias / 1000
    float tanHalfFovX;         // 1 / P00
    float pixelRadiusPerDepth; // min(1 / (W P00), 1 / (H P11)) at the render resolution
};

VsmLocalSmrtParams VsmLocalSmrtMake(float4 counts, float4 shape, float normalBias, float tanHalfFovX, float pixelRadiusPerDepth)
{
    VsmLocalSmrtParams p;
    p.rayCount = (uint)counts.x;
    p.samplesPerRay = (uint)counts.y;
    p.adaptiveRayCount = (uint)counts.z;
    p.frameIndex = (uint)counts.w;
    p.texelDitherScale = shape.x;
    p.extrapolateMaxSlope = shape.y;
    p.maxSlopeBias = shape.z;
    p.cotMaxRayAngle = shape.w;
    p.normalBias = normalBias;
    p.tanHalfFovX = tanHalfFovX;
    p.pixelRadiusPerDepth = pixelRadiusPerDepth;
    return p;
}

// One view's projection, about the light, so a ray needs no matrix: x = dot(colX, d) / w, y = dot(colY, d) / w, w = dot(fwd, d),
// z = zScale - zFn / w, for d = the point - the light. PerspectiveFovLH behind LookAtLH: zScale = f / (f - n), zFn = zScale x n.
struct VsmLocalProj
{
    uint   view;
    float3 colX;
    float3 colY;
    float3 fwd;
    float  scaleSq; // the projection's x scale, squared
    float  zScale;
    float  zFn;
};

// The D3D cube faces, VsmPointFaceProject's basis.
static const float3 kVsmCubeRight[6] = { float3(0, 0, -1), float3(0, 0, 1), float3(1, 0, 0), float3(1, 0, 0), float3(1, 0, 0), float3(-1, 0, 0) };
static const float3 kVsmCubeUp[6]    = { float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0), float3(0, 1, 0) };
static const float3 kVsmCubeFwd[6]   = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };

VsmLocalProj VsmCubeFaceProj(uint firstView, uint face, float zScale, float zFn)
{
    VsmLocalProj p;
    p.view = firstView + face;
    p.colX = kVsmCubeRight[face];
    p.colY = kVsmCubeUp[face];
    p.fwd = kVsmCubeFwd[face];
    p.scaleSq = 1.0f; // 90 degrees
    p.zScale = zScale;
    p.zFn = zFn;
    return p;
}

VsmLocalProj VsmSpotProj(uint view, float4x4 viewProj, float3 lightPos)
{
    VsmLocalProj p;
    p.view = view;
    p.colX = float3(viewProj._11, viewProj._21, viewProj._31);
    p.colY = float3(viewProj._12, viewProj._22, viewProj._32);
    p.fwd = float3(viewProj._14, viewProj._24, viewProj._34);
    p.scaleSq = dot(p.colX, p.colX);
    const float3 colZ = float3(viewProj._13, viewProj._23, viewProj._33);
    p.zScale = length(colZ);
    p.zFn = -(dot(colZ, lightPos) + viewProj._43);
    return p;
}

float3 VsmLocalUVZ(VsmLocalProj p, float3 d)
{
    const float w = max(dot(p.fwd, d), 1e-5f); // UE's by construction: a ray ends a quarter of the way short of the light
    return float3(0.5f + 0.5f * dot(p.colX, d) / w, 0.5f - 0.5f * dot(p.colY, d) / w, p.zScale - p.zFn / w);
}

// The stored depth at a shadow UV, from mip `minMip` to the first with a page. One texel, no sampler.
bool VsmLocalLoadDepth(uint view, float2 uv, uint minMip, StructuredBuffer<uint> PageTable, Texture2D Pool, out float depth)
{
    depth = 1.0f;
    [loop] for (uint level = minMip; level <= VSM_MAX_LEVEL; ++level)
    {
        const uint axis = VSM_L0_AXIS >> level;
        const uint2 page = min((uint2)(uv * (float)axis), axis - 1u);
        const uint entry = PageTable[VsmPageId(view, level, page.x, page.y)];
        if ((entry & VSM_RESIDENT_BIT_C) == 0u) { continue; }
        const uint phys = entry & VSM_PHYS_MASK_C;
        const float2 inPage = clamp((uv * (float)axis - float2(page)) * (float)VSM_PAGE_SIZE, 0.0f, (float)VSM_PAGE_SIZE - 1.0f);
        const int2 texel = int2(phys % VSM_POOL_PAGES_X, phys / VSM_POOL_PAGES_X) * (int)VSM_PAGE_SIZE + int2(inPage);
        depth = Pool.Load(int3(texel, 0)).r;
        return true;
    }
    return false;
}

// UE's FSMRTSingleRayState: the ray in one view.
struct VsmLocalFaceRay
{
    uint   view;
    float3 startUVZ;
    float3 stepUVZ;
    float  extrapolateSlope;
};

VsmLocalFaceRay VsmLocalFaceRayInit(VsmLocalProj p, float3 startD, float3 endD, float extrapolateMaxSlope, float depthBias, bool clampUV)
{
    VsmLocalFaceRay ray;
    ray.view = p.view;
    float3 start = VsmLocalUVZ(p, startD);
    ray.stepUVZ = VsmLocalUVZ(p, endD) - start;
    // The dither can move it slightly off the edge of the projection; not on a ray that goes over to another face.
    if (clampUV) { start.xy = saturate(start.xy); }
    start.z -= depthBias; // FLIP (UE: +=)
    ray.startUVZ = start;
    ray.extrapolateSlope = extrapolateMaxSlope * abs(ray.stepUVZ.z); // abs -- FLIP: their step toward the light is positive, ours negative
    return ray;
}

VsmSmrtSample VsmLocalFaceSample(VsmLocalFaceRay ray, uint minMip, float sampleTime, StructuredBuffer<uint> PageTable, Texture2D Pool)
{
    const float3 uvz = ray.startUVZ + ray.stepUVZ * sampleTime;
    VsmSmrtSample s;
    s.valid = false;
    s.sampleDepth = 0.0f;
    s.referenceDepth = uvz.z;
    s.extrapolateSlope = ray.extrapolateSlope;
    s.resetExtrapolation = false;
    if (all(uvz.xy == saturate(uvz.xy)))
    {
        float depth;
        if (VsmLocalLoadDepth(ray.view, uvz.xy, minMip, PageTable, Pool, depth))
        {
            s.valid = true;
            s.sampleDepth = depth;
        }
    }
    return s;
}

// UE's FSMRTTwoCubeFaceRayState: the march runs from the end of the ray to its start, so it starts in the end's face. A ray on one face (a spot,
// nearly every ray of a point) has face0 alone.
struct VsmLocalRayState
{
    VsmLocalFaceRay face0;
    VsmLocalFaceRay face1;
    uint minMip;
    bool sampleInFace1;
};

VsmSmrtSample VsmLocalRayFindSample(inout VsmLocalRayState st, float sampleTime, StructuredBuffer<uint> PageTable, Texture2D Pool)
{
    if (st.sampleInFace1)
    {
        const VsmSmrtSample s = VsmLocalFaceSample(st.face1, st.minMip, sampleTime, PageTable, Pool);
        if (s.valid) { return s; }
        VsmSmrtSample over = VsmLocalFaceSample(st.face0, st.minMip, sampleTime, PageTable, Pool);
        over.resetExtrapolation = true;
        st.sampleInFace1 = false;
        return over;
    }
    return VsmLocalFaceSample(st.face0, st.minMip, sampleTime, PageTable, Pool);
}

#define VSM_SMRT_MARCH_NAME VsmLocalRayCast
#define VSM_SMRT_RAY_STRUCT VsmLocalRayState
#define VSM_SMRT_MARCH_PARAMS , StructuredBuffer<uint> PageTable, Texture2D Pool
#define VSM_SMRT_FIND_SAMPLE(st, sampleTime) VsmLocalRayFindSample(st, sampleTime, PageTable, Pool)
#include "vsm_smrt_march.hlsli"
#undef VSM_SMRT_MARCH_NAME
#undef VSM_SMRT_RAY_STRUCT
#undef VSM_SMRT_MARCH_PARAMS
#undef VSM_SMRT_FIND_SAMPLE

// UE's GetTangentBasis (Frisvad, Duff et al.).
void VsmTangentBasis(float3 n, out float3 t, out float3 b)
{
    const float s = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0f + s * a * n.x * n.x, s * c, -s * n.x);
    b = float3(c, s + a * n.y * n.y, -n.y);
}

// The offset from the receiver along its normal (UE's VirtualShadowMapGetNormalBiasLength). 0.02 cm is theirs.
float VsmLocalNormalBias(VsmLocalSmrtParams prm, float distToCamera)
{
    return max(0.0002f, prm.normalBias * distToCamera * prm.tanHalfFovX);
}

// `isPoint`: the views firstView + face, zScale and zFn the faces'; else `spot`. `Pb` = the receiver offset along its normal, `N` its normal (ours:
// the geometry's, where theirs has only the shading normal and their note says that is the worse one). `viewDepth` = its depth in the camera,
// `lodBias` = the light's ResolutionLodBias, `minMip` = the mip the pixel asked for. 0 (in shadow) .. 1 (lit).
float VsmLocalTraceShadow(VsmLocalSmrtParams prm, bool isPoint, VsmLocalProj spot, uint firstView, float zScale, float zFn,
                          float3 lightPos, float sourceRadius, float3 Pb, float3 N, float viewDepth, float lodBias, uint minMip,
                          StructuredBuffer<uint> PageTable, Texture2D Pool)
{
    const float3 toLight = lightPos - Pb;
    const float dist2 = max(dot(toLight, toLight), 1e-8f);
    const float dist = sqrt(dist2);
    const float3 axis = toLight / dist;
    const float coneSin = sourceRadius / dist;

    VsmLocalProj receiver = spot;
    if (isPoint) { receiver = VsmCubeFaceProj(firstView, VsmCubeFace(-toLight), zScale, zFn); }

    // UE's ComputeDepthSlopeLocalUV, closed: their inverse-transpose takes the receiver's plane into UVz, and a plane stays a plane under a
    // perspective. With D = dot(N, toLight) the plane is N.x X + N.y Y + N.z Z + D = 0; over Z, with 1 / Z = (zScale - z) / zFn, dz/dx = (N.x / s) zFn / D.
    const float planeD = dot(N, toLight);
    const float planeDSafe = (abs(planeD) < 1e-6f) ? ((planeD < 0.0f) ? -1e-6f : 1e-6f) : planeD;
    const float2 depthSlopeUV = (2.0f * receiver.zFn / (planeDSafe * receiver.scaleSq)) * float2(dot(N, receiver.colX), -dot(N, receiver.colY));

    // UE's ComputeShadowResolutionScale: the camera pixel at the receiver, 0.1 cm at least, "since our shadows sadly don't actually have infinite
    // resolution near the camera". And its depth scale, radial so it does not jump between cube faces.
    const float resolutionScale = max(0.001f, viewDepth * prm.pixelRadiusPerDepth * exp2(lodBias));
    const float depthPerMetre = abs(receiver.zFn) / dist2;
    const float ditherScale = prm.texelDitherScale * resolutionScale;
    const float maxDitherSlope = prm.maxSlopeBias * ditherScale;
    const float maxDepthBias = prm.maxSlopeBias * resolutionScale * depthPerMetre;

    float3 coneT, coneB;
    VsmTangentBasis(axis, coneT, coneB);
    const float3 normalTangent = float3(dot(coneT, N), dot(coneB, N), dot(axis, N));
    const float normalZ = (abs(normalTangent.z) < 1e-4f) ? ((normalTangent.z < 0.0f) ? -1e-4f : 1e-4f) : normalTangent.z;
    const float2 depthSlopeTangent = -normalTangent.xy / normalZ;

    // A light of no size needs no samples along its rays.
    const int samples = (coneSin > 0.0f) ? (int)prm.samplesPerRay : 0;

    // The mip the receiver's own page is on: where the rays' reads start and whose texels the bias takes. Once a pixel: the dither moves a start by
    // about a texel, and per ray it was a walk of the table more than the rays' own reads (Pass_PointLights +50 % against the PCF, the same picture).
    const uint receiverMip = VsmResidentLevel(receiver.view, saturate(VsmLocalUVZ(receiver, Pb - lightPos).xy), minMip, PageTable);
    const float texels = VSM_VIRTUAL_RES / (float)(1u << receiverMip);

    const float2 pixelHash = frac(VsmSmrtHash2(Pb) + VsmSmrtR2(prm.frameIndex));
    float stepOffset = pixelHash.x;
    const uint rays = min(prm.rayCount, VSM_SMRT_MAX_RAYS);
    uint rayMissCount = 0u;
    uint r = 0u;
    [loop] for (; r < VSM_SMRT_MAX_RAYS; ++r)
    {
        if (r >= rays) { break; }
        const float2 e0 = frac(pixelHash + VsmSmrtR2(r));
        const float2 e1 = frac(pixelHash + VsmSmrtR2(r + rays));

        // The start moves along the receiver's plane (UE: 2x, and capped, the depth slope it earns).
        const float2 randomOffset = (e1 - 0.5f) * ditherScale;
        const float offsetZ = min(maxDitherSlope, 2.0f * max(0.0f, dot(depthSlopeTangent, randomOffset)));
        const float3 startD = (Pb - lightPos) + coneT * randomOffset.x + coneB * randomOffset.y + axis * offsetZ;

        // UE's ComputeOptimalSlopeBiasLocal. FLIP (UE: + dot): vsm_sample.hlsli says why.
        const float2 biasUV = saturate(VsmLocalUVZ(receiver, startD).xy);
        const float2 toTexelCentreUV = (floor(biasUV * texels) + 0.5f - biasUV * texels) / texels;
        const float depthBias = min(maxDepthBias, 2.0f * max(0.0f, -dot(depthSlopeUV, toTexelCentreUV)));

        bool rayHit = false;
        if (samples == 0)
        {
            // A light of no size: the march's one sample, at the start, without the march.
            VsmLocalProj startProj = spot;
            if (isPoint) { startProj = VsmCubeFaceProj(firstView, VsmCubeFace(startD), zScale, zFn); }
            const float3 uvz = VsmLocalUVZ(startProj, startD);
            float depth;
            if (VsmLocalLoadDepth(startProj.view, saturate(uvz.xy), receiverMip, PageTable, Pool, depth))
            {
                rayHit = depth < uvz.z - depthBias; // FLIP (UE: >, and + bias)
            }
        }
        else
        {
            const float2 lightUV = VsmSmrtDiskConcentric(e0) * coneSin;
            const float sinTheta2 = dot(lightUV, lightUV);
            const float cosTheta = sqrt(saturate(1.0f - sinTheta2));
            const float3 dir = coneT * lightUV.x + coneB * lightUV.y + axis * cosTheta;

            // UE's VirtualShadowMapGetClampedRayDistanceScaleLocal: the further the ray goes the more it bends against a shadow map, so it stops short.
            const float clampedEnd = dist * 0.75f * saturate(1.5f / (cosTheta + prm.cotMaxRayAngle * sqrt(sinTheta2)));
            const float3 endD = startD + dir * clampedEnd;

            VsmLocalRayState st;
            st.minMip = receiverMip;
            st.sampleInFace1 = false;
            if (isPoint)
            {
                const uint startFace = VsmCubeFace(startD);
                const uint endFace = VsmCubeFace(endD);
                st.sampleInFace1 = startFace != endFace;
                st.face0 = VsmLocalFaceRayInit(VsmCubeFaceProj(firstView, startFace, zScale, zFn), startD, endD, prm.extrapolateMaxSlope, depthBias, !st.sampleInFace1);
                st.face1 = st.face0;
                if (st.sampleInFace1)
                {
                    st.face1 = VsmLocalFaceRayInit(VsmCubeFaceProj(firstView, endFace, zScale, zFn), startD, endD, prm.extrapolateMaxSlope, depthBias, false);
                }
            }
            else
            {
                st.face0 = VsmLocalFaceRayInit(spot, startD, endD, prm.extrapolateMaxSlope, depthBias, true);
                st.face1 = st.face0;
            }
            rayHit = VsmLocalRayCast(st, samples, stepOffset, PageTable, Pool).validHit;
        }
        if (!rayHit) { ++rayMissCount; }

#if defined(VSM_SMRT_COMPUTE)
        if (rays > 1u && prm.adaptiveRayCount > 0u)
        {
            if (r == 0u)
            {
                if (WaveActiveAllTrue(!rayHit)) { break; }   // every lane missed
            }
            else if (r >= prm.adaptiveRayCount)
            {
                if (WaveActiveAllTrue(rayMissCount == 0u)) { break; } // every lane in umbra
            }
        }
#endif
        stepOffset = frac(stepOffset + 0.61803399f); // UE's EvolveSobolSeed: another offset for the next ray
    }
    return (float)rayMissCount / (float)max(min(r + 1u, rays), 1u);
}

#endif // VSM_SMRT_LOCAL_HLSLI
