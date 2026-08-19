#define BLOOM_CONV_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// P8C -- the three non-transform stages of convolution bloom: pack the frame, build the kernel, and
// read the result back out. The transform itself is bloom_fft_cs.hlsl.
//
//   stage 0 SETUP   HDR frame -> the padded complex grid, thresholded, downscaled.
//   stage 3 APERTURE the iris silhouette; stage 4 squares its transform into the kernel. The
//                   kernel is the DIFFRACTION PATTERN of the hole, not a drawing of one.
//   stage 2 RESOLVE the convolved grid -> mip 0 of the bloom UP chain, which is what the tonemap
//                   already samples. Convolution therefore needs no change in the tonemap at all:
//                   it is a different way of FILLING the same texture.
//
// COMPLEX PACKING, shared with the transform: `.xy` = R + iG, `.zw` = B + i0. Legal because
// convolution is linear and the kernel is real -- see the note in bloom_fft_cs.hlsl.
//
// ZERO PADDING IS NOT OPTIONAL. An FFT computes the CIRCULAR convolution, so without a border of
// zeros a streak leaving the right edge of the image reappears on the left. The image occupies
// `imageSize` of a larger `transformSize`; everything outside it is zero, and that margin is the
// pad. UE hit the same wall and CLAMP the pad when the transform would exceed their 4096 limit,
// accepting the wrap because the kernel's tails are faint (PostProcessFFTBloom.cpp) -- this keeps
// the margin explicit instead, because at this project's sizes it is affordable.

Texture2D HDRColor : register(t0);
// The thresholded, half-resolution scene -- ONE dispatch, no mip chain. This is what UE hand their
// flare pass (`QualitySceneDownsample`), and they read it at mip 0: the softness of a ghost comes
// from the BOKEH SPRITE they splat, not from a blurred source. The mip chain that used to be here
// was invented for a gather that was trying to make a copy of the frame look defocused, which is
// not what a ghost is.
Texture2D GhostSource : register(t1);
// The authored ghost sprites: 4x2 cells, greyscale shapes with chromatic rims. Art, not geometry --
// a procedural polygon cannot produce the coloured fringe that is the first thing anyone notices on
// a real ghost.
Texture2D GhostAtlas : register(t2);
RWTexture2D<float4> Grid : register(u0);        // the complex grid, transformSize
RWTexture2D<float4> BloomOut : register(u1);    // mip 0 of the bloom UP chain, resolve only
SamplerState gSmp : register(s0);

cbuffer BloomConvCB : register(b0)
{
    uint  convStage;        // 0 setup, 1 kernel, 2 resolve
    uint  exposureEnabled;
    uint2 transformSize;    // the padded power-of-two grid
    uint2 imageSize;        // how much of it the frame occupies; the rest is the zero pad
    uint2 sourceSize;       // the HDR image being read (setup) or written (resolve)
    float threshold;
    float softKnee;
    // The IRIS. `kernelRadius` is the pattern's core size in grid texels and the aperture radius is
    // its inverse; `blades` 0 means a round hole (halo, no rays), N even gives N rays and N odd
    // gives 2N.
    float kernelRadius;
    uint  blades;
    float bladeRotation;
    // RAY SHAPING, all of it applied to the IRIS rather than to the pattern. See the notes on
    // ApertureValue: straightness blends the polygon against a circle, feather apodizes the edge,
    // curvature bows each blade.
    float spokeStrength;   // 0 = round hole, no rays at all; 1 = full polygon
    float spokeLength;     // 1 = hard edge, longest rays; towards 0 the edge feathers and they shorten
    float spokeWidth;      // blade curvature; higher bows the blades and broadens each ray
    // The iris is squeezed in x by this much, so the pattern stretches in x -- which is what an
    // anamorphic lens does and where its horizontal streak comes from.
    float anamorphic;
    float anamorphicLength;
    // How differently the blue channel is convolved from red/green. The packing gives two
    // INDEPENDENT complex lanes -- (R + iG) and (B + i0) -- so B can have its own kernel for free.
    // R and G cannot be separated from each other; that would need a third transform.
    float chroma;
    // Ghosts: copies of the image mirrored through the screen centre. NOT part of the convolution
    // and they cannot be -- a convolution is shift-invariant by construction, and a ghost's
    // position depends on where the source sits relative to the centre. UE build theirs as a
    // separate pass for the same reason.
    uint  ghostCount;
    float ghostSpacing;
    float ghostIntensity;
    // Sprite radius as a fraction of the frame -- UE's `LensFlareBokehSize`, which they author as a
    // percent of viewport width and default to 3.
    float ghostBokeh;
    // Where the flare chain comes FROM, in UV, and whether it is on screen. A flare is
    // thrown by ONE bright source; giving the shader its position is what lets a ghost be
    // DRAWN rather than gathered out of the frame.
    float2 sunUV;
    float  sunOnScreen;
    // FFT-aperture kernel. `apertureScale` is the wavelength ratio for the lane being built --
    // the PSF scales with wavelength, so a longer wavelength means a SMALLER aperture. `psfLane`
    // selects which half of the packed kernel the squared transform lands in.
    float apertureScale;
    uint  psfLane;
};

// The exposure record, read to put `threshold` in the units the viewer sees. Same buffer and same
// arithmetic as tonemap_cs.hlsl and bloom_cs.hlsl -- a threshold measured against a different
// exposure than the image is not a threshold.
RWByteAddressBuffer ExposureValue : register(u2);

static const float3 kLumaWeights = float3(0.2126f, 0.7152f, 0.0722f);

float ExposureMultiplier()
{
    if (exposureEnabled == 0u) { return 1.0f; }
    const float ev100 = asfloat(ExposureValue.Load(0));
    if (isnan(ev100) || isinf(ev100)) { return 1.0f; }
    return (0.18f * 8.0f) / exp2(ev100);
}

// ---------------------------------------------------------------------------------------------
// STAGE 1 -- the aperture.
//
// Generated rather than imported, so the feature carries no content dependency and the blade count
// is a control instead of a bitmap. The profile is a bright core with a power-law skirt; the blade
// term modulates the radius by the distance to a regular polygon's edge, which is what makes the
// rays fall between the blades rather than along them.
//
// THE CENTRE IS AT TEXEL (0,0), NOT THE MIDDLE OF THE GRID. A convolution kernel's origin has to sit
// at the DC corner or the result comes out translated by half the grid -- the same wrap-around
// indexing the transform itself uses. Hence the fold below.
// ---------------------------------------------------------------------------------------------
// Returns the kernel for the two packed lanes: .x for (R + iG), .y for B. They are allowed to
// differ, and that difference IS the chromatic fringing -- a real lens does not smear every
// wavelength the same way.
// THE IRIS ITSELF. Everything a lens does to a point source follows from this shape: the star, how
// many rays it has, how long they are, how they colour. Fraunhofer says the point spread function is
// |FT{aperture}|^2, and this engine already owns the transform, so the honest kernel is to draw the
// hole and transform it rather than to draw the pattern and hope it matches.
//
// SIZE IS INVERSE. A wide aperture gives a TIGHT pattern: the first zero of a circular aperture of
// radius `a` in a grid of N sits at about N/(2a) output texels. `kernelRadius` is authored as the
// PSF's core size, so the aperture radius is N/(2*kernelRadius) -- and getting this backwards is the
// kind of mistake that still looks plausible, which is why it is spelled out.
//
// The aperture may be placed anywhere: |FT{A(x - x0)}|^2 = |FT{A}|^2, so a shift costs only a phase
// that the magnitude discards. Centred is simply easiest to reason about.
float ApertureValue(uint2 pixel)
{
    const float2 centre = float2(transformSize) * 0.5f;
    float2 d = float2(pixel) - centre;

    // ANAMORPHIC = AN ANAMORPHIC APERTURE, not a bar drawn into the result. Squeezing the iris in x
    // stretches the pattern in x, which is exactly where the horizontal streak comes from on a real
    // anamorphic lens.
    const float aspect = 1.0f + max(anamorphic, 0.0f);
    d.x *= aspect;

    // THE GRID IS NOT SQUARE and the transform is separable, so it imposes its own anisotropy: an
    // aperture of radius a gives a pattern N_x/a wide and N_y/a tall, in texels that are image
    // PIXELS in both directions. At 2560x1440 the grid is 1024x512, so a round hole came out
    // stretched 2:1 -- one horizontal ray across the whole screen while the diagonals stayed short.
    // Squashing the aperture by the same ratio cancels it exactly, and leaves `anamorphic` as the
    // only thing that can make the pattern anisotropic.
    d.y *= float(transformSize.x) / float(max(transformSize.y, 1u));

    // Inverse of the authored PSF core size, times the lane's wavelength ratio.
    const float apRadius = max(float(transformSize.x) /
                               (2.0f * max(kernelRadius, 1.0f)), 2.0f) / max(apertureScale, 1.0e-3f);

    const float round = length(d);
    const uint sides = blades;
    float inside = round;
    const float straight = saturate(spokeStrength);
    if (sides >= 3u && straight > 0.0f)
    {
        // Convex polygon by its half-planes: the distance is the WORST of the edge distances, so
        // one smoothstep below antialiases every edge at once. The apothem/circumradius factor
        // keeps the hole the size the radius says it is.
        //
        // CURVATURE bows each blade outward. A spike's angular width goes as one over the coherent
        // straight length, so a bowed blade -- which is what a real iris has -- shortens that length
        // and broadens the ray. At curvature 0 the edges are dead straight and the rays are at their
        // thinnest.
        const float step = 6.28318530718f / float(sides);
        const float apothem = cos(3.14159265f / float(sides));
        const float bow = max(spokeWidth, 0.0f) * 0.15f;
        float worst = -1.0e30f;
        [loop]
        for (uint i = 0u; i < sides; ++i)
        {
            float s, c;
            sincos(bladeRotation + step * float(i), s, c);
            const float along = (-d.x * s + d.y * c) / max(apRadius, 1.0f);
            const float bulge = bow * along * along;
            worst = max(worst, (d.x * c + d.y * s) / apothem - bulge * apRadius);
        }
        // STRAIGHTNESS: blend the polygon against the circle. A circle has no straight edges and
        // therefore throws no rays, which is why 0 here really is "no star" -- the control the user
        // expected, and the one the drawn kernel never actually gave.
        inside = lerp(round, worst, straight);
    }

    // EDGE APODIZATION, and this is what sets how FAR the rays reach. A hard edge is a step, and a
    // step's transform has the long 1/r^2 tails that are the rays themselves; feathering the edge
    // shortens them. It is exactly how an apodizing filter kills diffraction spikes on a real lens.
    // One texel is the floor -- below that the edge aliases and the ringing becomes noise.
    const float feather = max(1.0f, apRadius * 0.35f * (1.0f - saturate(spokeLength)));
    return 1.0f - smoothstep(apRadius - feather, apRadius + feather, inside);
}



// Bilinear read of the convolved grid. The grid is a UAV -- the chain never leaves
// UNORDERED_ACCESS, so there is no sampler to use and no hardware filtering to inherit. Point
// sampling it is what made the ghosts blocky: the grid is a quarter of the display width and a
// ghost magnifies it further, so one texel becomes a visible square.
float4 SampleGridBilinear(float2 gridUv)
{
    const float2 extent = float2(imageSize);
    const float2 p = gridUv * extent - 0.5f;
    const float2 f = frac(p);
    const int2 b = int2(floor(p));
    const int2 hi = int2(imageSize) - 1;

    const float4 s00 = Grid[clamp(b, int2(0, 0), hi)];
    const float4 s10 = Grid[clamp(b + int2(1, 0), int2(0, 0), hi)];
    const float4 s01 = Grid[clamp(b + int2(0, 1), int2(0, 0), hi)];
    const float4 s11 = Grid[clamp(b + int2(1, 1), int2(0, 0), hi)];
    return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// THE SPRITE IS THE IRIS. A ghost is a defocused image of the aperture -- photographs of flares show
// exactly the polygon of the blades, large and soft on a wide aperture, small and sharp stopped down
// -- so the same shape that builds the kernel draws the ghost. `o` is the offset from the ghost's
// centre in sprite radii.
float ApertureMask(float2 o)
{
    o.x *= 1.0f + max(anamorphic, 0.0f);
    const float round = length(o);
    const float straight = saturate(spokeStrength);
    if (blades < 3u || straight <= 0.0f) { return 1.0f - smoothstep(0.85f, 1.0f, round); }

    const float step = 6.28318530718f / float(blades);
    const float apothem = cos(3.14159265f / float(blades));
    float worst = -1.0e30f;
    [loop]
    for (uint i = 0u; i < blades; ++i)
    {
        float s, c;
        sincos(bladeRotation + step * float(i), s, c);
        worst = max(worst, (o.x * c + o.y * s) / apothem);
    }
    const float d = lerp(round, worst, straight);
    // A real ghost has a bright rim: the reflection is brightest where the cone of light grazes the
    // blade edge. Photographs show it on every one of them.
    const float body = 1.0f - smoothstep(0.85f, 1.0f, d);
    const float rim = smoothstep(0.55f, 0.9f, d) * (1.0f - smoothstep(0.9f, 1.0f, d));
    return body * 0.75f + rim * 0.6f;
}

// UE's LensFlareTints[8], verbatim (Scene.cpp). RGB is the tint; ALPHA encodes the ghost's scale
// about the screen centre through `(A * (N-1) - (N-1)/2) * GuardBandScale`, which is why the table
// is stored with the alpha rather than a separate position list.
// Deterministic per-ghost variation. A chain whose elements are evenly spaced and identically
// sized reads as a row of stickers; real ones are irregular. Hashed rather than random so a given
// ghost is the same every frame -- anything else would crawl.
float FlareHash(uint i, float seed)
{
    return frac(sin(float(i) * 12.9898f + seed * 78.233f) * 43758.5453f);
}

// Which sheet cell each ghost uses. The crescent (5) is pulled forward because it is the most
// recognisable shape in the set and sat seventh, where it only ever appeared at full ghost count.
static const uint kFlareShape[8] = { 0u, 5u, 2u, 1u, 3u, 6u, 4u, 7u };

static const float4 kFlareTints[8] = {
    float4(1.0f, 0.8f, 0.4f, 0.60f),
    float4(1.0f, 1.0f, 0.6f, 0.53f),
    float4(0.8f, 0.8f, 1.0f, 0.46f),
    float4(0.5f, 1.0f, 0.4f, 0.39f),
    float4(0.5f, 0.8f, 1.0f, 0.31f),
    float4(0.9f, 1.0f, 0.8f, 0.27f),
    float4(1.0f, 0.8f, 0.4f, 0.22f),
    float4(0.9f, 0.7f, 0.7f, 0.15f)
};

[numthreads(8, 8, 1)]
[RootSignature(BLOOM_CONV_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;

    if (convStage == 0u)
    {
        if (pixel.x >= transformSize.x || pixel.y >= transformSize.y) { return; }

        // Outside the image rectangle the grid is ZERO -- that is the pad, and it is what makes the
        // convolution linear rather than circular.
        if (pixel.x >= imageSize.x || pixel.y >= imageSize.y)
        {
            Grid[pixel] = 0.0f.xxxx;
            return;
        }

        // Four Karis-averaged taps, exactly as the pyramid's setup does and for the same reason:
        // this grid is a QUARTER of the source's resolution, so a single sample would ignore fifteen
        // of the sixteen source texels it covers and a moving glint would flicker in and out of the
        // set. The four taps do not cover all sixteen either, but they break the correlation that
        // makes the flicker visible, and the Karis weighting stops one blown-out sample dominating.
        const float2 uv = (float2(pixel) + 0.5f) / float2(imageSize);
        const float2 quarter = 0.25f / float2(imageSize);
        const float3 t0 = HDRColor.SampleLevel(gSmp, uv + float2(-quarter.x, -quarter.y), 0).rgb;
        const float3 t1 = HDRColor.SampleLevel(gSmp, uv + float2(quarter.x, -quarter.y), 0).rgb;
        const float3 t2 = HDRColor.SampleLevel(gSmp, uv + float2(-quarter.x, quarter.y), 0).rgb;
        const float3 t3 = HDRColor.SampleLevel(gSmp, uv + float2(quarter.x, quarter.y), 0).rgb;
        // Box, not Karis -- see the note in bloom_cs.hlsl: weighting by luma here removes the
        // highlight instead of clamping it, and the convolution has nothing left to spread.
        const float3 color = (t0 + t1 + t2 + t3) * 0.25f;

        // threshold < 0 = no threshold, which is UE's default -- see the note in bloom_cs.hlsl.
        // Convolution is where it matters most: the whole point of a kernel is that it describes
        // what the lens does to ALL the light reaching it.
        float3 lit = color;
        if (threshold >= 0.0f)
        {
            const float exposedLuma = dot(color, kLumaWeights) * ExposureMultiplier();
            const float amount = saturate((exposedLuma - threshold) * max(softKnee, 1.0e-4f));
            lit = amount * color;
        }


        Grid[pixel] = float4(lit.r, lit.g, lit.b, 0.0f);
        return;
    }

    if (convStage == 3u)
    {
        // ---- stage 3: the aperture, real, into the (R + iG) lane with G = 0 ----
        if (pixel.x >= transformSize.x || pixel.y >= transformSize.y) { return; }
        Grid[pixel] = float4(ApertureValue(pixel), 0.0f, 0.0f, 0.0f);
        return;
    }

    if (convStage == 4u)
    {
        // ---- stage 4: |transform|^2 -- the point spread function -- into one lane ----
        //
        // u0 is the TRANSFORMED APERTURE and u1 is the accumulator, which is not the bloom target
        // for this stage: the table is positional and the caller aims it somewhere else. Both are
        // RWTexture2D<float4>, so nothing about the binding changes.
        //
        // The lane is written REAL (value + 0i). The frequency-domain multiply is a per-channel
        // convolution only while the kernel is real -- see bloom_fft_cs.hlsl -- so an imaginary part
        // here would silently mix the two channels sharing the lane.
        if (pixel.x >= transformSize.x || pixel.y >= transformSize.y) { return; }
        const float4 s = Grid[pixel];
        const float psf = s.x * s.x + s.y * s.y;
        if (psfLane == 0u)
        {
            // First pass also clears the other lane, so the accumulator needs no separate zeroing.
            BloomOut[pixel] = float4(psf, 0.0f, 0.0f, 0.0f);
        }
        else
        {
            float4 d = BloomOut[pixel];
            d.z = psf;
            d.w = 0.0f;
            BloomOut[pixel] = d;
        }
        return;
    }

    // ---- stage 2: resolve ----
    if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }

    // The grid holds the convolved image in its `imageSize` rectangle; sample it across the bloom
    // target. Point-ish sampling through the UAV, so this is a straight scale-up of a signal that
    // is low frequency by construction.
    const float2 uv = (float2(pixel) + 0.5f) / float2(sourceSize);
    float4 c = SampleGridBilinear(uv);

    // ---- ghosts: the reflections between the elements of a real lens ----
    //
    // DRAWN, not gathered. A ghost is a defocused image of the APERTURE, and a flare chain is thrown
    // by ONE bright source, so the honest construction is: take the source's position, mirror and
    // scale it about the frame centre, and draw the iris there in the flare's tint. The reference
    // photographs show exactly that -- a string of coloured polygons along the line from the light
    // through the frame centre.
    //
    // A GATHER CANNOT DO THIS, and the failed attempt is worth recording. Sampling the frame over
    // the sprite's support needs about one tap per source texel underneath it -- thousands -- and
    // with twelve taps a point source produced twelve copies of itself. UE avoid it by SCATTERING,
    // one quad per source pixel textured with a bokeh sprite. Drawing the shape reaches the same
    // image without a rasteriser, because here the shape is known in closed form.
    //
    // Positions and tints are UE's: `LensFlareTints[8]` carries the scale in its ALPHA, as
    // (A*(N-1) - (N-1)/2) * GuardBandScale -- +1.40 +0.42 -0.56 -1.54 -2.66 -3.22 -3.92 -4.90 by
    // default, two on the same side of the centre and six mirrored.
    if (ghostCount > 0u && ghostIntensity > 0.0f && sunOnScreen > 0.5f)
    {
        // How bright the chain is, from the source itself, so a dimmer sun throws dimmer ghosts.
        // One tap on the thresholded half-resolution scene: everything under the flare threshold is
        // already gone there, so a sky with no bright source throws nothing at all.
        const float3 srcColor = max(GhostSource.SampleLevel(gSmp, sunUV, 0.0f).rgb, 0.0f.xxx);
        if (dot(srcColor, kLumaWeights) > 0.0f)
        {
            const uint count = min(ghostCount, 8u);
            const float aScale = float(count) - 1.0f;
            const float aBias = -aScale * 0.5f;
            const float guardBand = 2.0f;                       // UE's GuardBandScale
            const float aspect = float(sourceSize.x) / float(max(sourceSize.y, 1u));
            const float2 fromCentre = sunUV - 0.5f;

            // UE's, from LensFlareCompositePS: a circular falloff towards the screen border applied
            // twice at different radii, which is what makes a flare sit IN the frame rather than on
            // it. Evaluated once -- it depends on the output pixel, not on which ghost.
            const float2 screenPos = (uv - 0.5f) * 2.0f;
            const float disc = saturate(1.0f - dot(screenPos, screenPos)) *
                               saturate(1.0f - dot(screenPos * 0.8f, screenPos * 0.8f));

            // THE WHOLE CHAIN FADES WHEN YOU LOOK STRAIGHT AT THE SOURCE. Every element is placed
            // by scaling the source's offset from the centre, so as the source approaches the
            // centre the entire chain collapses onto it -- eight sprites stacked on the sun, which
            // is not what a lens does. Fading it out over the inner quarter of the frame is the
            // same thing happening for the same reason.
            const float chainFade = smoothstep(0.0f, 0.22f, length(fromCentre));

            [loop]
            for (uint g = 0u; g < count; ++g)
            {
                const float4 tint = kFlareTints[g];
                // `ghostSpacing` is OURS, not UE's: one multiplier over their whole table so the
                // chain can be pulled in or spread out without editing eight alphas. The jitter is
                // ours too -- their ladder is even, and an even chain reads as a row of stickers.
                const float jitterPos = 1.0f + (FlareHash(g, 1.7f) - 0.5f) * 0.55f;
                const float scale = (tint.a * aScale + aBias) * guardBand *
                                    max(ghostSpacing, 0.01f) * jitterPos;
                const float2 centre = fromCentre * scale + 0.5f;

                // A copy that is twice as wide is a quarter as bright: the same light over more
                // area. Same reasoning as UE's KernelAreaInverse on their splat. Size is jittered
                // for the same reason as position.
                const float jitterSize = 0.55f + FlareHash(g, 5.3f) * 1.15f;
                const float radius = max(ghostBokeh, 1.0e-4f) * (0.35f + 0.65f * abs(scale)) * jitterSize;
                float2 o = (uv - centre) / radius;
                o.x *= aspect;
                if (abs(o.x) > 1.0f || abs(o.y) > 1.0f) { continue; }

                // A WINDOW BEFORE THE ATLAS IS EVEN TOUCHED. The sheet has grid lines about three
                // pixels in from each cell edge, and the sprite is MINIFIED on screen -- a 443px
                // cell drawn at ~100px -- so a bilinear tap near the edge reaches them however wide
                // the inset is. Driving the sprite to zero before the quad's edge makes that
                // impossible rather than unlikely.
                const float edgeWindow = 1.0f - smoothstep(0.78f, 0.97f, max(abs(o.x), abs(o.y)));
                if (edgeWindow <= 0.0f) { continue; }

                const float2 kCells = float2(4.0f, 2.0f);
                const float kInset = 0.03f;      // ~13 px at 1774x887, four times the line width
                const float2 local = lerp(kInset.xx, (1.0f - kInset).xx, o * 0.5f + 0.5f);
                const uint shape = kFlareShape[g];
                const float2 cell = float2(float(shape % 4u), float(shape / 4u));
                // PICK THE MIP BY HOW SMALL THE SPRITE IS ON SCREEN. A compute shader has no
                // derivatives, so mip 0 was used unconditionally -- and a 443-texel cell drawn at
                // seventy pixels undersamples badly: thin features broke into dashes and straight
                // edges went jagged. Clamped so the tap never spreads far enough to reach the next
                // cell, which is what the inset is worth in mip levels.
                float atlasW, atlasH;
                GhostAtlas.GetDimensions(atlasW, atlasH);
                const float cellTexels = atlasW / kCells.x;
                const float screenPx = max(2.0f * radius * float(sourceSize.x), 1.0f);
                const float lod = clamp(log2(cellTexels / screenPx), 0.0f, 3.0f);
                float3 sprite = GhostAtlas.SampleLevel(gSmp, (cell + local) / kCells, lod).rgb;

                // BLACK ON THE SHEET IS NOT ZERO -- measured at about 0.3/255 with the odd 2-3 --
                // and the blend is ADDITIVE, so every one of those leaks as a faint square. Floor
                // it and renormalise, so background is exactly nothing and the shape keeps its
                // range.
                const float kBlackFloor = 0.02f;
                sprite = max(sprite - kBlackFloor, 0.0f.xxx) / (1.0f - kBlackFloor);
                if (dot(sprite, 1.0f.xxx) <= 0.0f) { continue; }

                // The sprite carries its own colour, so the tint is a LEAN rather than a multiply --
                // a straight multiply of a blue tint into a blue rim just makes mud.
                const float3 tinted = sprite * lerp(1.0f.xxx, tint.rgb, 0.5f);

                // EACH GHOST FADES BEFORE IT LEAVES THE FRAME, on its own centre rather than on the
                // pixel being shaded -- otherwise an element crosses the edge and pops.
                const float2 outside = max(max(-centre, centre - 1.0f), 0.0f.xx);
                const float ghostEdge = 1.0f - smoothstep(0.0f, 0.25f, length(outside));

                const float area = max(radius * radius, 1.0e-8f);
                c.xyz += srcColor * tinted *
                         (ghostIntensity * disc * chainFade * edgeWindow * ghostEdge * 1.0e-3f / area);
            }
        }
    }
    // UNIT MATCHING BETWEEN THE TWO METHODS, so `intensity` means the same thing in both and
    // switching method is not also a brightness change.
    //
    // The convolution CONSERVES energy: its kernel is normalised to sum 1, so the output carries
    // exactly the thresholded image's total light, redistributed. The pyramid does not -- its tent
    // upsample ADDS the levels together, so its output is roughly the level count times brighter.
    // Measured against bloom-off on sun_glint at the same intensity: pyramid mean +7.77/255,
    // convolution +0.58, a factor of ~13. kBloomMaxMips (8) is the structural reason for most of
    // that, and is used rather than the measured 13 because it is the thing that would change if
    // the pyramid ever gained or lost a level.
    const float kConvolutionGain = 8.0f;
    BloomOut[pixel] = float4(max(c.x, 0.0f) * kConvolutionGain,
                             max(c.y, 0.0f) * kConvolutionGain,
                             max(c.z, 0.0f) * kConvolutionGain, 1.0f);
}
