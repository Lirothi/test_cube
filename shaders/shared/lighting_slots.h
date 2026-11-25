// The lighting pass's registers (lighting_cs.hlsl; its b0 is lighting_cb.hlsli's PerFrame, which fog_scatter_cs.hlsl
// binds too), named once for the shader and for the C++ that fills its tables (SceneRenderer::Pass_Lighting) --
// architecture plan R5.3. A slot used to be a number on both sides with a comment saying which; now a new texture is
// a name here, the shader's TC_REGISTER(t, LIGHTING_T_...) and the C++ srv[LIGHTING_T_...] at once, and the root
// signature's table sizes are checked against the counts (lighting_cs.hlsl, beside LIGHTING_RS).
#ifndef TC_SHADERS_SHARED_LIGHTING_SLOTS_H
#define TC_SHADERS_SHARED_LIGHTING_SLOTS_H

// b: the per-frame constants (lighting_cb.hlsli).
#define LIGHTING_B_PER_FRAME 0

// t: the SRV table, in its order.
#define LIGHTING_T_GB0 0
#define LIGHTING_T_GB1 1
#define LIGHTING_T_GB2 2
#define LIGHTING_T_GB_VELOCITY 3
#define LIGHTING_T_DEPTH 4
#define LIGHTING_T_SHADOW_ATLAS 5      // S16: the SDSM moments in the same slot when EVSM is on
#define LIGHTING_T_VSM_PAGE_TABLE 6
#define LIGHTING_T_VSM_POOL 7
#define LIGHTING_T_GB_AUX 8
#define LIGHTING_T_CAUSTICS 9
#define LIGHTING_T_SKY_IRRADIANCE 10
#define LIGHTING_T_GTAO 11
#define LIGHTING_T_SKY_SPECULAR 12
#define LIGHTING_T_BRDF_LUT 13
#define LIGHTING_T_SKYBOX 14
#define LIGHTING_T_CLOUD_SHADOW 15
#define LIGHTING_T_SDSM_PARTITIONS 16
#define LIGHTING_T_SKY_VIS 17
#define LIGHTING_T_DDGI_IRRADIANCE 18
#define LIGHTING_T_DDGI_DEPTH 19
#define LIGHTING_T_DDGI_PROBE_STATE 20
#define LIGHTING_T_DDGI_FAR_IRRADIANCE 21
#define LIGHTING_T_DDGI_FAR_DEPTH 22
#define LIGHTING_T_DDGI_FAR_PROBE_STATE 23
#define LIGHTING_SRV_COUNT 24

// u: the light target.
#define LIGHTING_U_LIGHT_TARGET 0

// s: the sampler table, in its order.
#define LIGHTING_S_POINT_CLAMP 0
#define LIGHTING_S_COMPARISON_LINEAR 1
#define LIGHTING_S_LINEAR_WRAP 2
#define LIGHTING_S_LINEAR_CLAMP 3
#define LIGHTING_SAMPLER_COUNT 4

#endif
