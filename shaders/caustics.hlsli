#ifndef CAUSTICS_HLSLI
#define CAUSTICS_HLSLI

// Underwater caustics from a flipbook atlas, applied in the deferred lighting pass to every
// surface below the water line. The pattern modulates the SUN's irradiance, so it inherits the
// sun shadow and the surface cosine for free: a rock shading the seabed also kills its caustics.
//
// The ocean supplies the atlas, rectangular frame grid and number of used cells.
// Frames run left to right, then top to bottom. Empty cells after frameCount are never sampled.

struct CausticsParams
{
    float3 tint;
    float intensity;      // gain added to the direct sun term at a filament
    float scale;          // metres per tile of the pattern
    float time;           // seconds (the ocean's own clock, so caustics freeze with the waves)
    float speed;          // flipbook frames per second
    float waterLevel;     // world Y of the still water plane
    float depthFade;      // metres below the surface over which the effect fades out
    float surfaceFade;    // metres of fade-in right below the surface
    float upFacing;       // 0 = ignore the normal, 1 = full N.up gate
    float bias;           // pattern value that means "no gain"
    float dispersion;     // chromatic split in texels (0 = monochrome, and 3x cheaper)
    float layerBlend;     // second de-tiling layer, min-combined (0 = single layer)
    float pixelWorldScale;// world metres per screen pixel at one metre of view depth
    float2 grid;
    float frameCount;
    float maxLod;
    float2 frameTexels;
    float2 invFrameTexels;
};

// One flipbook frame lookup. The frame cell is addressed explicitly and the tile UV is inset by
// half a texel so bilinear taps never bleed into the neighbouring (i.e. temporally different)
// frame. LOD is computed by hand rather than from ddx/ddy: this runs in a compute shader, where
// derivatives need SM6.6, and the shader must still compile on the SM5 fallback path.
inline float CausticsSampleFrame(Texture2D atlas, SamplerState samp, CausticsParams p, float2 tileUv, float frame,
                                 float lod)
{
    const float index = fmod(fmod(floor(frame), p.frameCount) + p.frameCount, p.frameCount);
    const float2 cell = float2(fmod(index, p.grid.x), floor(index / p.grid.x));

    // Protect both mip levels of the trilinear lookup from neighbouring animation frames.
    const float2 inset = min(0.5 * exp2(ceil(lod)) * p.invFrameTexels, 0.5);
    const float2 local = frac(tileUv) * (1.0 - 2.0 * inset) + inset;
    return atlas.SampleLevel(samp, (cell + local) / p.grid, lod).r;
}

// Cross-faded pair of frames: without it the flipbook visibly steps at low speeds.
inline float CausticsSampleAnimated(Texture2D atlas, SamplerState samp, CausticsParams p, float2 tileUv,
                                    float frame, float lod)
{
    const float blend = frac(frame);
    const float a = CausticsSampleFrame(atlas, samp, p, tileUv, frame, lod);
    const float b = CausticsSampleFrame(atlas, samp, p, tileUv, frame + 1.0, lod);
    return lerp(a, b, blend);
}

// Two layers at different scales, combined with min(). A single layer repeats visibly once the
// camera can see several tiles at once; min() of two decorrelated layers keeps the filaments thin
// (an average would just wash them out) and hides the period.
inline float CausticsPattern(Texture2D atlas, SamplerState samp, CausticsParams p,
                             float2 baseUv, float frame, float lod)
{
    float pattern = CausticsSampleAnimated(atlas, samp, p, baseUv, frame, lod);
    if (p.layerBlend > 1.0e-3)
    {
        const float2 secondUv = baseUv * 0.63 + float2(0.37, 0.11) - 0.05 * p.time.xx;
        const float second = CausticsSampleAnimated(
            atlas, samp, p, secondUv, frame * 0.83 + 17.0, lod);
        pattern = lerp(pattern, min(pattern, second), p.layerBlend);
    }
    return pattern;
}

// Returns the multiplicative gain for the direct sun term (0 = unchanged).
// Cost: 2 texture taps, x2 with the second layer, x3 more with dispersion enabled.
inline float3 EvaluateCaustics(Texture2D atlas, SamplerState samp, CausticsParams p,
                               float3 positionWS, float3 normalWS, float viewDepth)
{
    const float depth = p.waterLevel - positionWS.y;
    if (p.intensity <= 0.0 || depth <= 0.0)
    {
        return 0.0.xxx;
    }

    // Fade in just under the surface (otherwise the waterline shows a hard bright edge) and out
    // with depth (the surface stops focusing usefully once the water column is deep).
    const float fadeIn = saturate(depth / max(p.surfaceFade, 1.0e-3));
    const float fadeOut = saturate(1.0 - depth / p.depthFade);
    // The pattern is projected straight down, so a near-vertical face receives a stretched,
    // meaningless projection. Weight it out rather than let it smear.
    const float facing = lerp(1.0, saturate(normalWS.y), p.upFacing);
    const float fade = fadeIn * fadeOut * fadeOut * facing;
    if (fade <= 1.0e-3)
    {
        return 0.0.xxx;
    }

    const float invScale = rcp(max(p.scale, 1.0e-3));
    const float2 baseUv = positionWS.xz * invScale;
    const float frame = p.time * p.speed;

    // Manual LOD: one screen pixel covers viewDepth*pixelWorldScale metres, stretched by the
    // surface slant; use the denser axis for rectangular frames.
    const float footprint = max(viewDepth, 0.0) * p.pixelWorldScale *
                            rcp(max(abs(normalWS.y), 0.25));
    const float lod = clamp(log2(max(footprint * invScale * max(p.frameTexels.x, p.frameTexels.y), 1.0)),
                            0.0, p.maxLod);

    float3 pattern;
    if (p.dispersion > 1.0e-3)
    {
        // Split the sample position per channel: the water surface disperses the sun into faint
        // coloured fringes along the filament edges.
        // Dispersion is already in FRAME TEXELS. Applying invScale again made the colour
        // separation vanish as the world-space tile grew (6x weaker at the default 6 m tile).
        const float2 offset = p.dispersion * p.invFrameTexels * float2(1.0, 0.7);
        pattern = float3(
            CausticsPattern(atlas, samp, p, baseUv + offset, frame, lod),
            CausticsPattern(atlas, samp, p, baseUv, frame, lod),
            CausticsPattern(atlas, samp, p, baseUv - offset, frame, lod));
    }
    else
    {
        pattern = CausticsPattern(atlas, samp, p, baseUv, frame, lod).xxx;
    }

    // Bias lets the dark cells between filaments remove a little light instead of only ever
    // adding it, which is closer to how a surface redistributes a fixed budget of sunlight.
    return (pattern - p.bias.xxx) * (p.intensity * fade) * p.tint;
}

#endif // CAUSTICS_HLSLI
