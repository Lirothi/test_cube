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
    uint spotCount;   uint pointLightIndex;  uint pointCount;         uint alphaMode;
    uint bounceOn;    float skyRayDistance;  uint _ddgiPad0;          uint _ddgiPad1;
}

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
    return normalize(mul(DdgiSphericalFibonacci(float(k - kDdgiFixedRays), float(kDdgiBlendRays)), (float3x3)rayRotation));
}

#endif // DDGI_CONSTANTS_HLSLI
