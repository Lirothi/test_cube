// The compose pass's registers (compose_cs.hlsl), named once for the shader and for the C++ that fills its tables
// (SceneRenderer::Pass_Compose) -- architecture plan R5.3, as lighting_slots.h. A new texture is a name here, the
// shader's TC_REGISTER(t, COMPOSE_T_...) and the C++ srv[COMPOSE_T_...] at once; the root signature's table sizes are
// checked against the counts (compose_cs.hlsl, beside COMPOSE_CS_RS).
#ifndef TC_SHADERS_SHARED_COMPOSE_SLOTS_H
#define TC_SHADERS_SHARED_COMPOSE_SLOTS_H

// b: the per-frame constants (compose_cs.hlsl PerFrame, written by reflected names: SceneComposeCBHandles).
#define COMPOSE_B_PER_FRAME 0

// t: the SRV table, in its order.
#define COMPOSE_T_LIGHT_TARGET 0      // HDR
#define COMPOSE_T_GB2 1               // DefaultLit emissive or foliage subsurface / transmission payload
#define COMPOSE_T_GB0 2               // albedo + metal in A
#define COMPOSE_T_GB1 3               // normal01 + roughness in A
#define COMPOSE_T_DEPTH 4             // R32F SRV of the DSV
#define COMPOSE_T_SKYBOX 5            // the skybox cubemap
#define COMPOSE_T_REFLECTION 6        // the filtered reflection (premultiplied)
#define COMPOSE_T_GB_AUX 7            // AO, indirect specular scale, shading model
#define COMPOSE_T_SHORE_WETNESS 8     // the persistent world-space shore wetness (R32_UINT, 0..65535)
#define COMPOSE_T_SKY_SPECULAR 9      // F8 GGX-prefiltered sky radiance (mip m <-> roughness m/(mips-1))
#define COMPOSE_T_SKY_IRRADIANCE 10   // F8 cosine-convolved sky irradiance (already divided by PI)
#define COMPOSE_T_BRDF_LUT 11         // F8 split-sum environment BRDF (RG16F)
#define COMPOSE_T_GTAO 12             // P6B dynamic AO at render resolution
#define COMPOSE_T_FOG_VOLUME 13       // the integrated froxel volume
#define COMPOSE_T_SKY_AERIAL 14       // B3 aerial perspective volume
#define COMPOSE_T_SKY_VIEW_LUT 15     // B6.2 SkyView LUT
#define COMPOSE_T_CLOUD 16            // plan C2: the resolved clouds
#define COMPOSE_T_CLOUD_DEPTH 17      // ...and their front depth
#define COMPOSE_T_CLOUD_HZB 18        // the furthest HZB, trace mode 0's bilateral upsampling
#define COMPOSE_T_SUN_TRANSMITTANCE 19 // the sky's transmittance LUT, the sun's disc
#define COMPOSE_T_SKY_VIS 20          // plan S: the RT sky visibility (lighting's LIGHTING_T_SKY_VIS)
#define COMPOSE_SRV_COUNT 21

// u: the scene colour (HDR).
#define COMPOSE_U_SCENE_COLOR 0

// s: the sampler table, in its order.
#define COMPOSE_S_LINEAR_CLAMP 0
#define COMPOSE_S_POINT_CLAMP 1
#define COMPOSE_SAMPLER_COUNT 2

#endif
