#define LENS_FLARE_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// P8C-2 step 5a -- the lens-flare BOKEH SCATTER, transcribed from UE's LensFlareBlurVS/PS
// (PostProcessLensFlares.usf). One instanced quad per TILE of the thresholded downsampled scene;
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

Texture2D FlareSource : register(t0);   // thresholded half-res scene (the bloom DOWN chain, mip 0)
Texture2D BokehSprite : register(t1);   // the iris shape, baked from the blade controls at load
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
};

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

[RootSignature(LENS_FLARE_RS)]
VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    // Triangle A: 0 left-top, 1 right-top, 2 left-bottom; B: 3 right-bottom, 4 left-bottom,
    // 5 right-top -- UE's own corner decode, verbatim.
    const float2 corner = float2((float)(vid % 2u), (vid > 1u && vid < 5u) ? 1.0f : 0.0f);

    const float2 tile = float2((float)(iid % tileCount.x), (float)(iid / tileCount.x));

    // The tile's FULL 2x2 source footprint, box-averaged. A single centre tap skipped half the
    // source texels: a wind-swaying palm frond flickered its gap pixels in and out of the missed
    // set, and the ghost image CRAWLED. Averaging the whole tile is the "small parent zone" that
    // the bokeh then inflates -- the softness still comes from the SPRITE, not from a blur.
    const float2 base = tile * tileSizeTexels;
    const float2 h = srcInvSize * 0.5f;
    const float2 uv00 = (base + 0.5f) * srcInvSize;
    const float2 srcUV = (tile + 0.5f) * tileSizeTexels * srcInvSize; // tile centre, for position
    float3 color = 0.25f * (
        FlareSource.SampleLevel(gSmp, uv00, 0.0f).rgb +
        FlareSource.SampleLevel(gSmp, uv00 + float2(2.0f * h.x, 0.0f), 0.0f).rgb +
        FlareSource.SampleLevel(gSmp, uv00 + float2(0.0f, 2.0f * h.y), 0.0f).rgb +
        FlareSource.SampleLevel(gSmp, uv00 + 2.0f * h, 0.0f).rgb);

    // The source's TRUE screen position, and the shrunk one the splat is placed at.
    const float2 screenPos = float2(srcUV.x * 2.0f - 1.0f, 1.0f - srcUV.y * 2.0f);
    float2 pos = screenPos * 0.5f;   // the guard-band shrink, positions only
    // UE's own mask, on the shrunk position, unchanged -- the gentle centre bias of their look.
    color *= DiscMask(pos);
    // PLUS a border fade on the TRUE screen position, which UE have no equivalent of and this
    // engine needs: the flare source is the ON-SCREEN image, so a source sitting on the frame
    // border is a disc CUT BY THE FRAME and every ghost in its chain came out a crescent
    // (observed). This takes the chain to zero exactly where the source stops being fully
    // visible. Full strength until 60% out so a sun merely high in the frame keeps its chain --
    // masking the true position with UE's disc instead cost 82% at 63% out, which killed the
    // ghosts outright (measured).
    const float border = max(abs(screenPos.x), abs(screenPos.y));
    color *= 1.0f - smoothstep(0.6f, 1.0f, border);

    // SOFT KNEE ABOVE THE THRESHOLD, which UE do not do -- their gate is binary, and a binary
    // gate passes a large region that is barely over the line at FULL strength. That is what put
    // recognisable mirrored copies of the sun's corona and of the water's glitter path in the sky
    // (observed): both are wide and only just above the threshold. Scaling by the EXCESS keeps
    // the hue, drops a source at the threshold to nothing and a source ten times over it to 90%,
    // so ghosts come from cores and not from haze.
    const float lum = color.r + color.g + color.b;   // UE: dot(rgb, 1)
    color.rgb *= max(lum - threshold, 0.0f) / max(lum, 1.0e-6f);
    const float alive = (lum < threshold) ? 0.0f : 1.0f;

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
