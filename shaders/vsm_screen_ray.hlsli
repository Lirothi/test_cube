// SCREEN-SPACE RAY CAST -- transcribed from Unreal's VirtualShadowMapScreenRayTrace.ush.
//
// WHAT IT IS, AND WHAT IT IS NOT. This is not a "contact shadow" bolted on beside the shadow map.
// It returns the LENGTH AT WHICH THE SMRT RAY SHOULD START: a short march away from the receiver,
// through the camera depth buffer, whose job is to get out of the region where the shadow map
// cannot decide anything. UE's own words for the cvar: "Length of the screen space shadow trace
// away from receiver surface (SMART SHADOW BIAS) before the VSM / SMRT lookup."
//
// So it buys two things at once. The obvious one is contact detail at a scale no shadow texel
// resolves. The quieter one is that the ambiguous band right at the surface -- where every bias
// argument in this engine has been fought -- is simply skipped rather than biased around.
//
// ONE PLEASANT SURPRISE: NOTHING FLIPS HERE. Every other transcription from UE in this engine has
// had to invert its depth comparisons, because their shadow maps are reverse-Z and ours are direct.
// This file reads the CAMERA depth buffer, and ours is ALREADY reverse-Z like theirs -- cleared to
// 0.0, `Material.h`'s default graphics DepthFunc is GREATER_EQUAL. So `SampleUVz.z < SampleDepth`
// means "the ray is behind geometry" here exactly as it does there, and the code below is a literal
// copy. (Only the shadow PSO overrides to LESS_EQUAL / clear 1.0 -- see RenderableObject.cpp.)
#ifndef VSM_SCREEN_RAY_HLSLI
#define VSM_SCREEN_RAY_HLSLI

// UE's SCREEN_RAY_SAMPLES.
static const uint VSM_SCREEN_RAY_MAX_SAMPLES = 16u;

// Returns the world-space length along `rayDir` at which the shadow-map ray should begin.
// `rayLength` is the full screen-trace length; returning it unchanged means the trace reached the
// end without finding anything, which is the common case and is what makes this a bias as well as
// a shadow.
//
// `worldToClip` is the camera's view-projection. `depthTex`/`pointSampler` are the scene depth.
float VsmScreenRayCast(float3 originWS, float3 rayDir, float rayLength, float dither, uint steps,
                       float4x4 worldToClip, Texture2D depthTex, SamplerState pointSampler)
{
    if (rayLength <= 0.0f || steps == 0u) { return 0.0f; }

    const float4 startClip = mul(float4(originWS, 1.0f), worldToClip);
    const float4 dirClip   = mul(float4(rayDir * rayLength, 0.0f), worldToClip);
    const float4 endClip   = startClip + dirClip;
    if (startClip.w <= 0.0f || endClip.w <= 0.0f) { return rayLength; }

    const float3 startScreen = startClip.xyz / startClip.w;
    const float3 endScreen   = endClip.xyz / endClip.w;
    const float3 stepScreen  = endScreen - startScreen;

    // NDC -> UV. UE fold this into View.ScreenPositionScaleBias; written out here because this
    // engine has no such uniform, and the y flip is the whole content of it.
    const float3 startUVz = float3(startScreen.x * 0.5f + 0.5f, 0.5f - startScreen.y * 0.5f,
                                   startScreen.z);
    const float3 stepUVz  = float3(stepScreen.x * 0.5f, -stepScreen.y * 0.5f, stepScreen.z);

    const uint n = min(steps, VSM_SCREEN_RAY_MAX_SAMPLES);
    const float stepT = 1.0f / (float)n;
    // Dither the phase, exactly as UE do: without it the four samples land on the same relative
    // offsets for every pixel and the result bands.
    float sampleTime = (dither - 0.5f) * stepT + stepT;

    // The receiver's OWN depth, used to skip self-intersection. UE compare it exactly, which is
    // only sound because the depth is point-sampled -- a filtered sample would never be equal.
    const float startDepth = depthTex.SampleLevel(pointSampler, startUVz.xy, 0).r;

    // Bounded by a literal; `n` can only end it early. See vsm_smrt.hlsli on why a loop bound is
    // never taken straight from a constant buffer.
    [loop] for (uint i = 0u; i < VSM_SCREEN_RAY_MAX_SAMPLES; ++i)
    {
        if (i >= n) { break; }

        const float3 sampleUVz = startUVz + stepUVz * sampleTime;
        if (any(sampleUVz.xy < 0.0f) || any(sampleUVz.xy > 1.0f)) { break; } // left the screen

        const float sampleDepth = depthTex.SampleLevel(pointSampler, sampleUVz.xy, 0).r;
        if (sampleDepth != startDepth)
        {
            // NO FLIP: reverse-Z on both sides, so a SMALLER z is FARTHER. The ray point being
            // farther than the surface stored here means it has gone behind geometry.
            if (sampleUVz.z < sampleDepth)
            {
                // Back up a step and a half before handing over, so the shadow-map ray does not
                // start inside the occluder the screen ray just found.
                return rayLength * max(0.0f, sampleTime - 1.5f * stepT);
            }
        }
        sampleTime += stepT;
    }

    // Reached the end without going behind anything: the shadow-map ray starts a full screen-ray
    // length out. This is the "smart bias" half -- the near band is skipped even when nothing
    // occludes.
    return rayLength;
}

#endif // VSM_SCREEN_RAY_HLSLI
