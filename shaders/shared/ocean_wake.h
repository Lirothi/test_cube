// WakeCB's fields ONCE (architecture plan R5.3): HLSL's `cbuffer WakeCB` (ocean_wake_cs.hlsl) and C++'s WakeCB
// (ocean/OceanWake.cpp) spell OCEAN_WAKE_FIELDS. A field list, not a struct: HLSL reads a cbuffer as globals; TC_INIT
// is the C++ mirror's initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow
// the next field). Include shared/shader_types.h first. New fields are APPENDED.
// The arrays' lengths are the parameters: HLSL passes its MAX_HULLS, MAX_FRONTS, FRONT_PIECES, MAX_FRONT_POINTS, C++
// OceanWake's counts (kMaxHulls, kMaxFronts, kMaxFrontPoints / kFrontPiece, kMaxFrontPoints) as numbers.
#ifndef TC_SHADERS_SHARED_OCEAN_WAKE_H
#define TC_SHADERS_SHARED_OCEAN_WAKE_H

#define OCEAN_WAKE_FIELDS(MAX_HULLS, MAX_FRONTS, FRONT_PIECES, MAX_FRONT_POINTS) \
    uint  Resolution; /* texels a side (the FFT's size) */                                                            \
    float Texel; /* metres a texel */                                                                                 \
    float CenterX; /* window centre, world XZ */                                                                      \
    float CenterZ;                                                                                                    \
    int   ShiftX; /* the re-anchor: this texel was (coord + shift) last step */                                       \
    int   ShiftY;                                                                                                     \
    float DeltaTime; /* seconds this step */                                                                          \
    uint  Clear; /* 1 = last step's field is not read (the first step, a teleport, a new texel size) */               \
    float Gravity; /* m/s2 */                                                                                         \
    float DampingK2; /* 1/s per (rad/m)^2: the shortest wave's damping over its k^2 */                                \
    float Strength; /* the hull pressure's scale (1 = the head its draft stands for) */                               \
    uint  HullCount;                                                                                                  \
    float EdgeTexels; /* the absorber's width */                                                                      \
    float EdgeRate; /* 1/s at the border, falling as the square of the distance in */                                 \
    uint  ShoreValid; /* 0 = no shore map this frame: deep water everywhere */                                        \
    float ShoreCamHeight; /* the shore depth map: waterDepth = lerp(zNear, zFar, raw) - camHeight */                  \
    float ShoreCenterX; /* shore depth map window (the surf sim's, ShoreDepthUV's): v flipped */                      \
    float ShoreCenterZ;                                                                                               \
    float ShoreInvExtent; /* uv = offset * invExtent + 0.5 */                                                         \
    float ShoreZNear;                                                                                                 \
    float ShoreZFar;                                                                                                  \
    /* 1/s: a long wave in the shallows (d < wavelength / 20) runs down at it as d goes to 0; 0 = none */             \
    float BreakRate;                                                                                                  \
    float FoamFade; /* 1/s the foam runs down at */                                                                   \
    float FoamDiffusion; /* nu dt / texel^2: the foam's spreading a step (a 4-neighbour Laplacian) */                 \
    float WashStrength; /* foam a metre of head change under a moving hull leaves (0 on a step that may not wash) */  \
    float WashThreshold; /* m/s of head change below which a hull leaves no white water */                            \
    float CrestStrength; /* Crest's Wave Foam Strength: 5 dt x this x saturate(coverage - det J) a step */            \
    /* Crest's Wave Foam Coverage (0.55: det J of a Gerstner wave at the Stokes limit, a k = 0.44) */                 \
    float CrestCoverage;                                                                                              \
    float FoamFadeDense; /* 1/s per unit of foam: dense white water runs down fast, the lace it leaves slowly */      \
    uint  FrontCount; /* the breaking bow waves' fronts this step (OceanWake::StepFronts), two a boat */              \
    /* the share of that foam a head RISING lays (a hull's bow arriving), of what a head falling does */              \
    float WashBow;                                                                                                    \
    float Pad3;                                                                                                       \
    /* Per hull, three float4: (posX, posZ, fwdX, fwdZ), (scale, u0, v0, 1/cellU), (1/cellV, along, across, */        \
    /* offset). */                                                                                                    \
    float4 Hulls[MAX_HULLS * 3];                                                                                      \
    /* (min x, min z, max x, max z) world of each piece of a front: its FRONT_PIECE segments, the band's reach in */  \
    float4 FrontBounds[MAX_FRONTS * FRONT_PIECES];                                                                    \
    /* (points, which side of the line is outward: +1 its left normal, -1 its right, the residue's rate: x foam / */  \
    /* half width a second, the points over which the residue comes in from the hull: 0 = whole from the first) */    \
    float4 FrontInfo[MAX_FRONTS];                                                                                     \
    /* (x, z, the band's foam, half width m), newest (on the hull) first */                                           \
    float4 FrontPoints[MAX_FRONTS * MAX_FRONT_POINTS];

#endif
