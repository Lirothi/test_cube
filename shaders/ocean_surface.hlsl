// TWO WHOLE SURFACES IN ONE MATERIAL, chosen by OCEAN_SHORE_RUNUP (named on 2026-09-12 by what
// they are; "legacy"/"classic" was the old name of the first, and it is the one in use):
//   0 (the engine's default,        — ocean_surface_surf_sim.hlsli: the surf-sim surface -- classic
//      "--ocean-surf-sim-shore")      depth-map damping, the surf sim's injected height and foam
//                                     fields, the contact foam; grown from the pre-rework surface.
//   1 ("--ocean-runup-shore")       — ocean_surface_runup.hlsli: the run-up shore stack -- run-up
//                                     sheet with a travelling front, anchored swash, contact foam
//                                     with the torn dither edge, the SDF, the sink.
// The compiled-in default here is 1 because OceanRenderable passes OCEAN_SHORE_RUNUP=0 explicitly
// for the surf-sim surface (g_shoreRunup false) and nothing for the run-up one.
#ifndef OCEAN_SHORE_RUNUP
#define OCEAN_SHORE_RUNUP 1
#endif

#if !OCEAN_SHORE_RUNUP
#include "ocean_surface_surf_sim.hlsli"
#else
#include "ocean_surface_runup.hlsli"
#endif // OCEAN_SHORE_RUNUP
