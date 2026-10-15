#define LENS_FLARE_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// P8C-2 step 5a -- the lens-flare BOKEH SCATTER, transcribed from UE's LensFlareBlurVS/PS
// (PostProcessLensFlares.usf). One instanced quad per TILE of the downsampled scene;
// a quad whose tile is dark collapses to zero size and rasterizes nothing, so the pass costs what
// the bright pixels cost, not what the screen costs.
//
// The output is the actual DEFOCUSED IMAGE of the actual bright sources: each surviving quad is
// the iris polygon, sized `kernelSizePx`, coloured by its tile, additively accumulated. Ghost
// SHAPES therefore come from the scene itself -- two suns give two chains for free, and no
// authored sprite atlas exists anywhere. The composite that turns this image into the ghost chain
// is in bloom_conv_cs.hlsl's resolve.
//
// THE GUARD BAND: positions are shrunk x0.5 about the screen centre (UE's GuardBandScale = 2), so
// a source OFF the screen edge still lands inside the flare target and its ghosts still sweep
// through the frame. The composite un-does the scale and pays back the energy (x4).

Texture2D FlareSource : register(t0);   // the HDR scene, pre-exposed, full resolution
Texture2D BokehSprite : register(t1);   // the iris shape, baked from the blade controls at load
Texture2D<float> SceneDepth : register(t2); // the scene's depth (reverse-Z, sky 0): the art split's sky test
SamplerState gSmp : register(s0);

cbuffer LensFlareCB : register(b0)
{
    uint2  tileCount;        // tiles across the source viewport
    float2 flareRTSize;      // the scatter target, in pixels
    float2 srcInvSize;       // 1 / GhostSource texture size, texels
    float  tileSizeTexels;   // source texels per tile
    float  kernelSizePx;     // bokeh sprite width in flare-target pixels
    float  threshold;        // collapse tiles whose dot(rgb,1) is below this
    float  kernelAreaInverse;// 1 / max(1, kernelSizePx^2) -- same light over more area
    // The ART split (BloomSettings::convSunGhostScale): the sun's own share of the ghosts.
    // sunDirView: xyz the direction TO the sun, view space, w 1 = in view; sunShare: x the scale, y the
    // sun's radius (rad: the disc and the sky round it), zw the projection's _11 / _22.
    float4 sunDirView;
    float4 sunShare;
};

// 1 on the sun -- the disc and the bright SKY round it -- 0 elsewhere: bloom_conv_cs.hlsl's
// SunShareMask term for term, so a glint on geometry next to the sun keeps the common share.
float SunShareMask(float2 uv)
{
    if (sunDirView.w < 0.5f) { return 0.0f; }
    if (SceneDepth.SampleLevel(gSmp, uv, 0.0f) > 0.0f) { return 0.0f; }
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float3 dir = normalize(float3(ndc / sunShare.zw, 1.0f));
    const float angle = acos(clamp(dot(dir, sunDirView.xyz), -1.0f, 1.0f));
    const float r = max(sunShare.y, 1.0e-4f);
    return 1.0f - smoothstep(0.5f * r, r, angle);
}

// UE's screen-border falloff; the scatter fades a tile's CONTRIBUTION by where its source sits.
float DiscMask(float2 screenPos)
{
    const float x = saturate(1.0f - dot(screenPos, screenPos));
    return x * x;
}

struct VSOut
{
    float4 position : SV_POSITION;
    noperspective float2 uv : TEXCOORD0;
    noperspective float3 color : TEXCOORD1;
};

// P8C-3: mirrored by the DrawInstanced call -- 6 vertices a quad, this many quads an instance.
// UE call it GLensFlareQuadsPerInstance.
//
// P8C-5: THESE MUST STAY ABOVE THE [RootSignature] ATTRIBUTE. Declaring them between the attribute
// and VSMain detaches it from the function: the shader then compiles CLEANLY with no root
// signature, the pipeline silently fails to build, and the material lives on with a null PSO --
// every gate still says the ghosts are on, the draw is still issued, and the flare target comes
// out exactly zero. That cost most of a session to find.
static const uint kFlareQuadsPerInstance = 4u;
static const uint kFlareVertsPerInstance = 6u * kFlareQuadsPerInstance;

[RootSignature(LENS_FLARE_RS)]
VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    // P8C-3: FOUR QUADS PER INSTANCE, which is UE's GLensFlareQuadsPerInstance = 4. Drawing one
    // quad per instance meant four times their instance count for the same splat -- the only place
    // in the whole flare comparison where they were ahead of us. 24 vertices an instance, the low
    // bits pick the corner and the high bits the quad.
    const uint quad = vid / 6u;
    const uint v = vid % 6u;
    const uint tileIndex = iid * kFlareQuadsPerInstance + quad;
    // The last instance is partial whenever the tile count is not a multiple of four; those quads
    // collapse to zero size below rather than reading outside the grid.
    const bool inGrid = tileIndex < (tileCount.x * tileCount.y);

    // Triangle A: 0 left-top, 1 right-top, 2 left-bottom; B: 3 right-bottom, 4 left-bottom,
    // 5 right-top -- UE's own corner decode, verbatim.
    const float2 corner = float2((float)(v % 2u), (v > 1u && v < 5u) ? 1.0f : 0.0f);

    const uint safeIndex = inGrid ? tileIndex : 0u;
    const float2 tile = float2((float)(safeIndex % tileCount.x), (float)(safeIndex / tileCount.x));

    // The tile's footprint, sampled at four spread points rather than one. A single centre tap
    // skipped most of the texels a tile covers: a wind-swaying palm frond flickered its gap
    // pixels in and out of the missed set, and the ghost image CRAWLED. This is the "small parent
    // zone" the bokeh then inflates -- the softness still comes from the SPRITE, not from a blur.
    const float2 srcUV = (tile + 0.5f) * tileSizeTexels * srcInvSize; // tile centre, for position
    const float2 q = 0.25f * tileSizeTexels * srcInvSize;
    float3 color = 0.25f * (
        FlareSource.SampleLevel(gSmp, srcUV + float2(-q.x, -q.y), 0.0f).rgb +
        FlareSource.SampleLevel(gSmp, srcUV + float2( q.x, -q.y), 0.0f).rgb +
        FlareSource.SampleLevel(gSmp, srcUV + float2(-q.x,  q.y), 0.0f).rgb +
        FlareSource.SampleLevel(gSmp, srcUV + float2( q.x,  q.y), 0.0f).rgb);

    // The art split: the sun disc's tiles at their own share, before the gate (a sun dimmed under the
    // threshold drops out of the ghosts, as any source that dim would).
    color *= lerp(1.0f, max(sunShare.x, 0.0f), SunShareMask(srcUV));

    // The source's screen position, shrunk by the guard band -- where the splat is placed.
    const float2 screenPos = float2(srcUV.x * 2.0f - 1.0f, 1.0f - srcUV.y * 2.0f);
    float2 pos = screenPos * 0.5f;
    // UE's mask, on the shrunk position (LensFlareBlurVS: `OutColor.rgb *= DiscMask(OutPosition.xy)`),
    // BEFORE the threshold test, as theirs is.
    color *= DiscMask(pos);

    // UE's gate, verbatim: BINARY on dot(rgb, 1) in the pre-exposed units the source holds. A tile
    // under it collapses to a zero-size quad; one over it splats at full strength. With the sun's disc
    // physical (thousands of times the sky around it) the disc is what clears it, so the ghost is the
    // disc defocused through the iris -- the bokeh shape -- and not the sky's bright zone.
    const float lum = color.r + color.g + color.b;
    const float alive = (lum < threshold || !inGrid) ? 0.0f : 1.0f;

    const float2 cornerDelta = corner - 0.5f;
    pos += float2(2.0f * cornerDelta.x * kernelSizePx / flareRTSize.x,
                  -2.0f * cornerDelta.y * kernelSizePx / flareRTSize.y) * alive;

    VSOut o;
    o.position = float4(pos, 0.0f, 1.0f);
    o.uv = corner;
    o.color = color * kernelAreaInverse;
    return o;
}

[RootSignature(LENS_FLARE_RS)]
float4 PSMain(VSOut i) : SV_Target
{
    // The sprite is single-channel: the iris polygon with its bright rim, baked at load from the
    // blade controls (which is where `blades`/`bladeRotation` live now that the aperture-PSF
    // kernel is retired).
    const float sprite = BokehSprite.Sample(gSmp, i.uv).r;
    return float4(i.color * sprite, 1.0f);
}
