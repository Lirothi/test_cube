#ifndef SSR_TRACE_HIZ_HLSLI
#define SSR_TRACE_HIZ_HLSLI

// P6C step 6 -- hierarchical screen-space trace over the CLOSEST depth pyramid.
//
// This is Unreal's `TraceHZB` (Shaders/Private/HZBTracing.ush) transcribed into this engine's
// conventions, not an approximation of it. The stackless traversal is the whole point: instead of
// taking N fixed steps and hoping one of them lands inside a surface, the ray walks tile boundaries
// and changes RESOLUTION as it goes -- coarse while it is skipping empty space, fine once something
// is in the way. Two consequences, and the second is the reason this is a quality change and not
// only a speed one:
//
//   * empty space costs a handful of iterations instead of dozens of taps, and
//   * a thin surface between two fixed steps CANNOT be missed, because a tile that contains any
//     surface is never skipped -- the traversal descends into it instead.
//
// WHY THIS NEEDS THE CLOSEST PYRAMID (max device Z under reversed-Z, so the NEAREST surface in the
// tile). The skip test is "is the ray in front of everything this tile contains?" If the pyramid
// stored the furthest depth instead, a tile holding a near railing and a far wall would report the
// wall, the ray would be judged clear, and it would tunnel straight through the railing. GTAO wants
// exactly the opposite tile summary, which is why there are two pyramids and not one.
//
// Requires (host shader provides): `ssr_trace_logmarch.hlsli` for SSRHit / BuildSsrHit / the shared
// ssr* constants, plus HzbClosest, gSmpPoint, proj, hzbSize, hzbInvSize, hzbMipCount and
// DepthToViewZ_Fast.

// UE's shipping defaults for reflection HZB traces:
//   r.Lumen.Reflections.HierarchicalScreenTraces.MaxIterations           = 50
//   r.Lumen.Reflections.HierarchicalScreenTraces.RelativeDepthThickness  = 0.005
// The thickness is RELATIVE to the hit's depth (a pixel one metre away is thinner than the same
// pixel a hundred metres away), which is the part the fixed `ssrThicknessVS` of the other two
// tracers gets wrong at both ends of the range.
static const float ssrHizMaxIterations = 50.0f;
static const float ssrHizRelativeThickness = 0.005f;

// Self-intersection slack near the ray's origin, and how fast it fades. See the long note at the
// use site -- this is the one place where following UE literally produced a visible artefact.
static const float ssrHizStartSlack = 0.02f;   // push the tile 2% further, in VIEW units
static const float ssrHizSlackTexels = 3.0f;   // gone this many mip-0 texels from the origin

// LET THE TRAVERSAL DESCEND BELOW MIP 0, ONTO THE DEPTH BUFFER ITSELF (UE's
// HZB_TRACE_INCLUDE_FULL_RES_DEPTH). Mip 0 is HALF the depth resolution, so without this the hit
// is quantised to a half-res tile. In a still frame that is invisible; under DLSS it is not,
// because the sub-pixel jitter moves the ray's start, a moved start falls into the neighbouring
// tile, and the hit then jumps a WHOLE TILE instead of a little. The accumulator averages those
// discrete answers and paints horizontal bands across the reflection -- exactly what the log march
// does not do, since its bisection is continuous in the start position. UE call the extra level an
// "incoherent branch on the inner tracing loop", which is the cost: the level is only reached by
// rays that already have something in front of them.
#define ssrHizFullResDepth 1

float2 SsrLineBoxIntersect(float3 rayOrigin, float3 rayEnd, float3 boxMin, float3 boxMax)
{
    const float3 invRayDir = 1.0f / (rayEnd - rayOrigin);
    const float3 firstPlane = (boxMin - rayOrigin) * invRayDir;
    const float3 secondPlane = (boxMax - rayOrigin) * invRayDir;
    const float3 closestPlane = min(firstPlane, secondPlane);
    const float3 furthestPlane = max(firstPlane, secondPlane);

    float2 result;
    result.x = max(closestPlane.x, max(closestPlane.y, closestPlane.z));
    result.y = min(furthestPlane.x, min(furthestPlane.y, furthestPlane.z));
    return saturate(result);
}

// `startUv` / `startDeviceZ` are the REFLECTOR's own screen position, passed in rather than
// re-projected from Pv. Re-projecting would reintroduce the projection round-trip error exactly
// where the trace is most sensitive to it -- the first tile -- and the caller already has both.
SSRHit TraceSSR_Hiz(float3 Pv, float3 Nv, float2 startUv, float startDeviceZ)
{
    SSRHit outv;
    outv.uv = startUv;
    outv.visibility = 0.0f;
    outv.hit = 0;

    const float3 unitPositionFrom = normalize(Pv);
    const float3 pivot = normalize(reflect(unitPositionFrom, Nv));

    // Same rejection the other two tracers apply, so the technique A/B differs only in the search.
    if (pivot.z <= 0.0f)
    {
        return outv;
    }

    const float3 rayStart = float3(startUv, startDeviceZ);

    // End of the ray. When the reflection travels back toward the camera it must stop short of the
    // z == 0 plane, or the projection divide flips its sign and the segment lands somewhere
    // meaningless. UE clamp to 99% of the way there; so do we.
    float endDistance = ssrMaxDistanceVS;
    if (pivot.z < 0.0f)
    {
        endDistance = min(-0.99f * Pv.z / pivot.z, ssrMaxDistanceVS);
    }

    const float3 endVS = Pv + pivot * endDistance;
    const float4 endClip = mul(float4(endVS, 1.0f), proj);
    const float3 endNdc = endClip.xyz / max(endClip.w, 1e-6f);
    float3 rayEnd = float3(endNdc.x * 0.5f + 0.5f, -endNdc.y * 0.5f + 0.5f, endNdc.z);

    // Clip the segment to the screen box so `t` runs 0..1 over the visible part of the ray. The
    // traversal's termination test is `t < 1`, so this is also what makes "left the screen" a
    // normal exit rather than a special case checked per iteration.
    const float2 boxT = SsrLineBoxIntersect(rayStart, rayEnd, float3(0.0f, 0.0f, 0.0f), float3(1.0f, 1.0f, 1.0f));
    rayEnd = rayStart + (rayEnd - rayStart) * boxT.y;

    float3 rayDir = rayEnd - rayStart;
    // Every plane intersection below divides by rayDir. UE let a zero component produce an
    // infinity and rely on min() to discard it, but a component that is zero while the plane
    // distance is also zero gives 0/0 = NaN, and one NaN poisons the whole traversal. An axis-
    // aligned reflection is not exotic (a flat surface under an axis-aligned view), so clamp the
    // magnitude instead: 1e-7 in UV units is far past the last tile boundary, i.e. still "never".
    rayDir.x = (abs(rayDir.x) < 1e-7f) ? ((rayDir.x < 0.0f) ? -1e-7f : 1e-7f) : rayDir.x;
    rayDir.y = (abs(rayDir.y) < 1e-7f) ? ((rayDir.y < 0.0f) ? -1e-7f : 1e-7f) : rayDir.y;

    // Which of the two XY boundary planes of a tile the ray is heading for.
    const float2 dirSign = float2((rayDir.x < 0.0f) ? -1.0f : 1.0f, (rayDir.y < 0.0f) ? -1.0f : 1.0f);
    const float2 floorOffset = 0.5f * (dirSign + 1.0f);

    const float maxMip = max((float)hzbMipCount - 1.0f, 0.0f);
    // Mip -1 is not a pyramid level; it is the depth buffer, at full resolution.
    const float baseMip = (ssrHizFullResDepth != 0) ? -1.0f : 0.0f;

    float mipLevel = baseMip;
    float t = 0.0f;
    float3 rayUv = rayStart;

    // Step out of the starting tile WITHOUT a hit test. The reflector is itself the closest surface
    // in its own tile, so the first test would always report an intersection with the pixel the ray
    // left from.
    {
        const float2 mipTexel = (mipLevel < 0.0f) ? SSR_TRACE_INV_SCREEN_SIZE
                                                  : exp2(mipLevel) * hzbInvSize;
        const float2 mipRes = (mipLevel < 0.0f) ? SSR_TRACE_SCREEN_SIZE
                                                : exp2(-mipLevel) * hzbSize;
        const float2 uvOffset = 0.005f * mipTexel * dirSign;
        const float2 xyPlane = (floor(rayUv.xy * mipRes) + floorOffset) * mipTexel + uvOffset;
        const float2 planeT = (xyPlane - rayStart.xy) / rayDir.xy;
        t = min(planeT.x, planeT.y);
        rayUv = rayStart + t * rayDir;
    }

    float lastAboveSurfaceT = t;
    float iterations = 0.0f;

    // Stackless HZB traversal. Ascend a level every time a tile is skipped whole, descend one every
    // time a tile might contain the hit; the loop ends when it descends past mip 0 (a hit candidate
    // at full pyramid resolution) or runs out of ray.
    while (mipLevel >= baseMip && iterations < ssrHizMaxIterations && t < 1.0f)
    {
        const bool fullRes = (mipLevel < 0.0f);
        const float2 mipTexel = fullRes ? SSR_TRACE_INV_SCREEN_SIZE : exp2(mipLevel) * hzbInvSize;
        const float2 mipRes = fullRes ? SSR_TRACE_SCREEN_SIZE : exp2(-mipLevel) * hzbSize;
        const float2 uvOffset = 0.005f * mipTexel * dirSign;
        const float2 xyPlane = (floor(rayUv.xy * mipRes) + floorOffset) * mipTexel + uvOffset;

        // At mip -1 the "tile" is one depth texel, so its closest depth IS the depth.
        float tileZ = fullRes ? SSR_TRACE_READ_DEPTH(rayUv.xy)
                              : HzbClosest.SampleLevel(gSmpPoint, rayUv.xy, mipLevel).x;

        // SELF-INTERSECTION SLACK, and the two places UE's version does not survive transcription.
        //
        // The problem is real: mip 0 is HALF the depth resolution, so the tile a grazing ray is
        // leaving contains texels up to one full-res texel NEARER than the reflector itself, and
        // without slack every reflection off a flat floor hits that floor immediately.
        //
        // (1) UE fade the slack over the first 10% of the RAY (`saturate(t * 10)`). That is fine
        //     for a short probe ray and wrong here: a grazing reflection across a mirror floor gets
        //     clipped to the whole screen, so 10% of it is metres of world, and every genuine
        //     contact inside that stretch is suppressed. On screen that is PETER PANNING -- the
        //     reflection detaches from the base of whatever is standing on the floor. The artefact
        //     is LOCAL to the origin, so fade over TEXELS TRAVELLED, which is what it is about.
        // (2) UE scale device Z (`tileZ *= 0.99`). Device Z is not linear in distance, so a fixed
        //     percentage of it is a hair near the camera and a chasm at range. Convert once and the
        //     slack means the same thing everywhere: viewZ' = viewZ * (1 + slack), which in device
        //     terms is deviceZ' = depthA + (deviceZ - depthA) / (1 + slack).
        // APPLIES AT EVERY LEVEL, mip -1 included. Skipping it on the full-resolution level looked
        // principled -- a single depth texel summarises nothing, so there is no tile to be wrong
        // about -- and it put black bands across the whole floor: a GRAZING ray runs within a
        // texel of the surface it left for a long way, so it re-intersects that surface whatever
        // the resolution. The slack is about the ray hugging its own reflector, not about tile
        // summarisation, which is why it fades over distance travelled and not over mip level.
        const float texelsTravelled = length((rayUv.xy - rayStart.xy) * hzbSize);
        const float slack = ssrHizStartSlack *
                            (1.0f - saturate(texelsTravelled / ssrHizSlackTexels));
        tileZ = depthA + (tileZ - depthA) / (1.0f + slack);

        const float3 boundaryPlanes = float3(xyPlane, tileZ);
        float3 planeT = (boundaryPlanes - rayStart) / rayDir;
        // Under reversed-Z a ray whose device Z is increasing is moving TOWARD the camera and can
        // never cross the tile's depth plane from in front. 1.0 = "not on this segment".
        planeT.z = (rayDir.z < 0.0f) ? planeT.z : 1.0f;

        float intersectionT = min(planeT.x, planeT.y);

        // Reversed-Z again: greater device Z = nearer. "Above the surface" therefore means the ray
        // is still in FRONT of everything this tile contains, so the tile can be skipped whole.
        const bool aboveSurface = rayUv.z > tileZ;
        bool skippedTile = aboveSurface;

        intersectionT = min(intersectionT, planeT.z);
        skippedTile = skippedTile && (intersectionT != planeT.z);

        if (skippedTile)
        {
            lastAboveSurfaceT = intersectionT;
        }

        t = aboveSurface ? intersectionT : t;
        rayUv = rayStart + min(t, 1.0f) * rayDir;
        mipLevel += skippedTile ? 1.0f : -1.0f;
        // UE rely on the pyramid having more levels than the traversal can climb. Clamping is free
        // and strictly conservative: a tile smaller than the level the ray asked for costs an extra
        // iteration, it can never hide a surface.
        mipLevel = min(mipLevel, maxMip);

        iterations += 1.0f;
    }

    if (mipLevel >= baseMip || t >= 1.0f)
    {
        // Ran out of ray, or out of iterations, without descending to a candidate.
        outv.uv = rayUv.xy;
        return outv;
    }

    // The traversal descended past its base level, so `rayUv` is on the surface it found. When the
    // base level is -1 that surface came from the DEPTH BUFFER and this is already exact; when it
    // is 0 the answer is quantised to a half-resolution tile (see the note on ssrHizFullResDepth).
    const float tileZ = (ssrHizFullResDepth != 0)
        ? SSR_TRACE_READ_DEPTH(rayUv.xy)
        : HzbClosest.SampleLevel(gSmpPoint, rayUv.xy, 0).x;
    const float hitViewZ = DepthToViewZ_Fast(tileZ);
    const float rayViewZ = DepthToViewZ_Fast(rayUv.z);
    const float thicknessVS = ssrHizRelativeThickness * max(hitViewZ, 1e-4f);
    const float depthDiff = rayViewZ - hitViewZ;

    if (depthDiff >= thicknessVS)
    {
        // The ray passed BEHIND the surface by more than that surface is thick -- it went through a
        // gap the screen has no information about, so this is a miss, not a hit.
        outv.uv = (rayStart + lastAboveSurfaceT * rayDir).xy;
        return outv;
    }

    // Same fades as the other two techniques, so a technique A/B compares the SEARCH and nothing
    // else. depthDiff can be slightly negative (the descent stops just in front of the surface);
    // BuildSsrHit clamps the ratio, so that costs no visibility.
    return BuildSsrHit(pivot, unitPositionFrom, Pv, rayUv.xy, tileZ, thicknessVS, max(depthDiff, 0.0f));
}

#endif // SSR_TRACE_HIZ_HLSLI
