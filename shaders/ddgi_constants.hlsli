// Interior lighting plan D: the probe volume's per-frame constants, shared by ddgi_trace_cs.hlsl and
// ddgi_update_cs.hlsl. Mirrors DdgiConstants in sources/app/scene/SceneRenderer_Gi.cpp (row-major; a
// matrix, then rows of 4).
#ifndef DDGI_CONSTANTS_HLSLI
#define DDGI_CONSTANTS_HLSLI

cbuffer DdgiConstants : register(b0)
{
    float4x4 rayRotation;   // this update's random rotation of the Fibonacci directions
    int4 volumeBase;        // xyz the volume's first world cell, w its generation
    float4 volumeParams;    // x spacing (m), y hysteresis, z ray reach (m), w depth clamp (m)
    uint4 schedule;         // x 1/N of the probes per frame, y this frame's phase, z probes this frame, w frame seed
    float3 sunDirWS;  float ambientIntensity;
    float3 lightRgb;  float skyIrradianceScale;
    uint tlasIndex;   uint geomInfoIndex;    uint rayUavIndex;        uint irradianceIndex;
    uint depthIndex;  uint stateIndex;       uint skyIrradianceIndex; uint spotLightIndex;
    uint spotCount;   uint pointLightIndex;  uint pointCount;         uint twoSidedSheets; // gi.twoSidedSheets: a two-sided material's back is its other face
    uint bounceOn;    float skyRayDistance;  uint meshOpacity;        uint traceFlags; // rt.meshOpacity: the meshes' cutout opacity; kDdgiTrace*
    uint blendRays;      // the blended rays this update traces (gi.rays: 16-64, a multiple of 16)
    uint fixedRayPeriod; // a probe traces its fixed rays every this many of its updates (gi.fixedRayPeriod; 1 = every one)
    uint updateIndex;    // the volume's update cycle (frames / divisor): each probe sees it rise by one per update
    uint usageIndex;     // the trace's bindless index of this cascade's usage stamps (ddgi_usage_cs.hlsl)
    uint frameStamp;     // this frame's stamp, as ddgi_usage_cs wrote it
    uint sleepPeriod;    // an idle probe updates one turn in this many (gi.sleepPeriod; 1 = every turn)
    uint _ddgiPad1;
    uint _ddgiPad2;
}

// How long a probe stays AWAKE after a pixel last read it (ddgi_usage_cs stamps every pixel's probes within 16 frames).
static const uint kDdgiAwakeFrames = 32u;
bool DdgiProbeIdle(uint usedStamp) { return frameStamp - usedStamp > kDdgiAwakeFrames; }
// Whether this turn of probe `probeIndex` is skipped: an idle probe (placed, not just moved -- the callers ask) updates
// one turn in sleepPeriod, staggered over the probes by a hash, so every frame takes its share. The trace and the
// update ask this one function with the same stamp, so they skip the same probes.
bool DdgiProbeSleeps(uint probeIndex, bool idle)
{
    const uint period = max(sleepPeriod, 1u);
    return idle && period > 1u && ((updateIndex + ((probeIndex * 2246822519u) >> 20)) % period) != 0u;
}

// traceFlags: what a hit is lit by -- the sky through a ray (else the open sky), the lamps, the infinite bounce's
// visibility test (else the probes blend without their depth moments) -- and whether a probe inside geometry skips its
// blended rays and its blending (gi.skipInactive, the A/B; the trace and the update read it alike).
static const uint kDdgiTraceSkyRay = 1u;
static const uint kDdgiTraceLights = 2u;
static const uint kDdgiTraceBounceVisibility = 4u;
static const uint kDdgiTraceSkipInactive = 8u;

uint DdgiBlendRayCount() { return clamp(blendRays, 16u, kDdgiBlendRays); }

DdgiVolumeDesc DdgiVolumeFromConstants()
{
    DdgiVolumeDesc v;
    v.base = volumeBase.xyz;
    v.generation = uint(volumeBase.w);
    v.spacing = volumeParams.x;
    return v;
}

// The ray `k` of this update, the same in the trace and the update: a fixed ray (k < kDdgiFixedRays) is its own
// Fibonacci set, unrotated (RTXGI DDGIGetProbeRayDirection); a blended one this update's rotation of the other set.
float3 DdgiRayDirection(uint k)
{
    if (k < kDdgiFixedRays) { return DdgiSphericalFibonacci(float(k), float(kDdgiFixedRays)); }
    return normalize(mul(DdgiSphericalFibonacci(float(k - kDdgiFixedRays), float(DdgiBlendRayCount())), (float3x3)rayRotation));
}

// Whether this update of probe `probeIndex` traces its fixed rays: one in `fixedRayPeriod` of its updates, staggered
// over the probes so every frame takes its share (the trace's answer and the update's are this one function).
bool DdgiFixedRaysDue(uint probeIndex)
{
    const uint period = max(fixedRayPeriod, 1u);
    // A hash of the slot, not the slot: one frame's probes share their index mod the divisor, and a plain sum put
    // all of them on the same update.
    return ((updateIndex + ((probeIndex * 2654435761u) >> 20)) % period) == 0u;
}

#endif // DDGI_CONSTANTS_HLSLI
