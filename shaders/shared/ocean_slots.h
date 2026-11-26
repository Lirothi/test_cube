// The ocean surface's registers (ocean_surface.hlsl: ocean_surface_runup.hlsli and ocean_surface_surf_sim.hlsli, with
// ocean_surface_common.hlsli), named once for both surfaces and for the C++ that fills the table
// (OceanRenderable::RecordGraphics) -- architecture plan R5.3, as lighting_slots.h. The run-up surface's table is the
// first OCEAN_RUNUP_SRV_COUNT slots of the surf-sim one; the root signatures' sizes are checked against the counts.
#ifndef TC_SHADERS_SHARED_OCEAN_SLOTS_H
#define TC_SHADERS_SHARED_OCEAN_SLOTS_H

// b: OceanCB (written by reflected names), the transparent pass's GlassView (shared/glass_view.h, read as a struct),
// and the surf-sim surface's WaterMaskCB.
#define OCEAN_B_OCEAN 0
#define OCEAN_B_GLASS_VIEW 1
#define OCEAN_B_WATER_MASK 2

// t: the SRV table, in its order.
#define OCEAN_T_DISPLACEMENT 0
#define OCEAN_T_PREV_DISPLACEMENT 1
#define OCEAN_T_FOAM_TURBULENCE 2
#define OCEAN_T_SCENE_COLOR 3
#define OCEAN_T_SKYBOX 4
#define OCEAN_T_DISTANT_ROUGHNESS 5
#define OCEAN_T_FOAM_DETAIL 6
#define OCEAN_T_FOAM_ALBEDO 7
#define OCEAN_T_FOAM_UNDERWATER 8
#define OCEAN_T_FOAM_TRAIL 9
#define OCEAN_T_SHORE_FOAM_MASK 10       // the run-up surface's breakup mask, the surf-sim surface's contact foam: one texture
#define OCEAN_T_SHORE_FOAM_ALBEDO 11
#define OCEAN_T_SCENE_DEPTH 12
#define OCEAN_T_SHORE_DEPTH 13
#define OCEAN_T_SHORE_SDF 14
#define OCEAN_T_RETIRED_15 15            // ocean G-buffer plan S4 retired it; the table is positional, a stand-in fills it
#define OCEAN_T_SURF_SIM_WAVE 16
#define OCEAN_T_SURF_SIM_FOAM 17
#define OCEAN_T_SKY_SPECULAR 18
#define OCEAN_T_SKY_IRRADIANCE 19
#define OCEAN_T_FOG_VOLUME 20            // volumetric fog plan A5
#define OCEAN_T_CLOUD_SHADOW 21          // plan C3
#define OCEAN_T_SHADOW_ATLAS 22          // the sun's cascade / SDSM atlas (the glass pass's transports)
#define OCEAN_T_VSM_PAGE_TABLE 23
#define OCEAN_T_VSM_POOL 24
#define OCEAN_T_SDSM_PARTITIONS 25
#define OCEAN_RUNUP_SRV_COUNT 26
#define OCEAN_T_WAKE 26                  // the boat wake's field (ocean/OceanWake)
#define OCEAN_T_WAKE_FOAM 27
#define OCEAN_T_WATER_MASK_COLUMNS 28
#define OCEAN_T_WAKE_FOAM_DETAIL 29
#define OCEAN_T_WATER_MASK_CROSSINGS 30
#define OCEAN_SURF_SIM_SRV_COUNT 31

// u: the shore wetness stamp map.
#define OCEAN_U_WETNESS_STAMP 0

// s: the sampler table, in its order.
#define OCEAN_S_LINEAR_WRAP 0
#define OCEAN_S_LINEAR_CLAMP 1
#define OCEAN_S_POINT_CLAMP 2
#define OCEAN_S_ANISO_WRAP 3
#define OCEAN_S_SHADOW_COMPARE 4
#define OCEAN_SAMPLER_COUNT 5

#endif
