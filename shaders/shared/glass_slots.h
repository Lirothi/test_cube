// The glass pass's registers (glass.hlsl), named once for the shader and for the C++ that fills its tables -- the
// per-pass t0..t14 (rendering/renderables/GlassPassSrvs.cpp, published once a pass), the draw's normal map in it, and
// the glass material slot's own t15..t19 (GBufferRenderable::RenderTranslucent). Architecture plan R5.3, as
// lighting_slots.h; the root signatures' table sizes are checked against the counts (glass.hlsl, beside GLASS_RS).
#ifndef TC_SHADERS_SHARED_GLASS_SLOTS_H
#define TC_SHADERS_SHARED_GLASS_SLOTS_H

// b: the draw's GlassParams and the pass's per-view GlassView (shared/glass_view.h).
#define GLASS_B_PARAMS 0
#define GLASS_B_VIEW 1

// t: the per-pass table, in its order (GlassPassSrvs).
#define GLASS_T_SCENE_OPAQUE 0       // the opaque scene copy
#define GLASS_T_SHADOW_ATLAS 1       // the SUN atlas: depth, or the EVSM4 moments under S16
#define GLASS_T_SPOT_SHADOW 2
#define GLASS_T_SKYBOX 3
#define GLASS_T_POINT_LIGHTS 4
#define GLASS_T_SPOT_LIGHTS 5
#define GLASS_T_NORMAL_MAP 6         // the DRAW's: the per-pass table leaves it empty
#define GLASS_T_REFLECTION 7         // S15b: the off-screen RT glass reflection (premultiplied)
#define GLASS_T_POINT_SHADOW 8       // B3: point shadow faces, 6 per light
#define GLASS_T_VSM_PAGE_TABLE 9
#define GLASS_T_VSM_POOL 10
#define GLASS_T_SKY_SPECULAR 11      // P5: the prefiltered sky
#define GLASS_T_FOG_VOLUME 12        // plan A5: the integrated froxel volume
#define GLASS_T_SDSM_PARTITIONS 13   // S15
#define GLASS_T_LOCAL_SHADOW_POOL 14 // plan S7
#define GLASS_PASS_SRV_COUNT 15

// ...and a glass material slot's own after them (GLASS_MATERIAL).
#define GLASS_T_MAT_ALBEDO 15
#define GLASS_T_MAT_MR 16
#define GLASS_T_MAT_TRANSMISSION 17
#define GLASS_T_OPAQUE_DEPTH 18      // the opaque depth copy (the refraction's landing)
#define GLASS_T_SKY_VIS 19           // plan S: the RT sky visibility
#define GLASS_MATERIAL_SRV_COUNT 20

// s: the sampler table, in its order (the material slot's MatSampler only in GLASS_MATERIAL).
#define GLASS_S_LINEAR 0
#define GLASS_S_SHADOW 1
#define GLASS_S_ENV 2
#define GLASS_SAMPLER_COUNT 3
#define GLASS_S_MATERIAL 3
#define GLASS_MATERIAL_SAMPLER_COUNT 4

#endif
